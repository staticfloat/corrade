/*
    This file is part of Corrade.

    Copyright © 2007, 2008, 2009, 2010, 2011, 2012, 2013, 2014
              Vladimír Vondruš <mosra@centrum.cz>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "AbstractManager.h"

#include <cstring>
#include <algorithm>
#include <functional>
#include <sstream>

#ifndef CORRADE_TARGET_WINDOWS
#if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
#include <dlfcn.h>
#endif
#else
#include <windows.h>
#undef interface
#define dlsym GetProcAddress
#define dlerror GetLastError
#define dlclose FreeLibrary
#endif

#include "Corrade/PluginManager/AbstractPlugin.h"
#include "Corrade/Utility/Assert.h"
#include "Corrade/Utility/Directory.h"
#include "Corrade/Utility/Configuration.h"

#include "configure.h"

using namespace Corrade::Utility;

namespace Corrade { namespace PluginManager {

const int AbstractManager::Version = CORRADE_PLUGIN_VERSION;

auto AbstractManager::initializeGlobalPluginStorage() -> GlobalPluginStorage& {
    static GlobalPluginStorage* const plugins = new GlobalPluginStorage;

    /* If there are unprocessed static plugins for this manager, add them */
    if(staticPlugins()) {
        for(auto it = staticPlugins()->begin(); it != staticPlugins()->end(); ++it) {
            StaticPlugin* const staticPlugin = *it;

            /* Load static plugin metadata */
            Resource r("CorradeStaticPlugin_" + staticPlugin->plugin);
            std::istringstream metadata(r.get(staticPlugin->plugin + ".conf"));

            /* Insert plugin to list */
            const auto result = plugins->plugins.insert(std::make_pair(staticPlugin->plugin, new Plugin(staticPlugin->plugin, metadata, staticPlugin)));
            CORRADE_INTERNAL_ASSERT(result.second);

            /* Add aliases to the list (only the ones that aren't already there
               are added) */
            for(auto it = result.first->second->metadata._provides.begin(); it != result.first->second->metadata._provides.end(); ++it) {
                #ifndef CORRADE_GCC47_COMPATIBILITY
                plugins->aliases.emplace(*it, *result.first->second);
                #else
                plugins->aliases.insert({*it, *result.first->second});
                #endif
            }
        }

        /** @todo Assert dependencies of static plugins */

        /* Delete the array to mark them as processed */
        delete staticPlugins();
        staticPlugins() = nullptr;
    }

    return *plugins;
}

std::vector<AbstractManager::StaticPlugin*>*& AbstractManager::staticPlugins() {
    static std::vector<StaticPlugin*>* _staticPlugins = new std::vector<StaticPlugin*>();

    return _staticPlugins;
}

#ifndef DOXYGEN_GENERATING_OUTPUT
void AbstractManager::importStaticPlugin(std::string plugin, int _version, std::string interface, Instancer instancer, void(*initializer)(), void(*finalizer)()) {
    CORRADE_ASSERT(_version == Version,
        "PluginManager: wrong version of static plugin" << plugin + ", got" << _version << "but expected" << Version, );
    CORRADE_ASSERT(staticPlugins(),
        "PluginManager: too late to import static plugin" << plugin, );

    staticPlugins()->push_back(new StaticPlugin{std::move(plugin), std::move(interface), instancer, initializer, finalizer});
}
#endif

#if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
AbstractManager::AbstractManager(std::string pluginDirectory): _plugins(initializeGlobalPluginStorage()) {
    setPluginDirectory(std::move(pluginDirectory));
}
#else
AbstractManager::AbstractManager(std::string): _plugins(initializeGlobalPluginStorage()) {}
#endif

AbstractManager::~AbstractManager() {
    /* Unload all plugins associated with this plugin manager */
    #if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
    std::vector<std::map<std::string, Plugin*>::iterator> removed;
    #endif
    for(auto it = _plugins.plugins.begin(); it != _plugins.plugins.end(); ++it) {
        /* Plugin doesn't belong to this manager */
        if(it->second->manager != this) continue;

        #if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
        /* Try to unload the plugin (and all plugins that depend on it) */
        const LoadState loadState = unloadRecursiveInternal(*it->second);

        /* Schedule it for deletion, if it is not static, otherwise just
           disconnect this manager from the plugin and finalize it, so another
           manager can take over it in the future. */
        if(loadState == LoadState::Static) {
            it->second->manager = nullptr;
            it->second->staticPlugin->finalizer();
        } else removed.push_back(it);
        #else
        /* In NaCl newlib and Emscripten there are only static plugins */
        it->second->manager = nullptr;
        it->second->staticPlugin->finalizer();
        #endif
    }

    /* Remove all non-static aliases associated with this manager. After the
       previous step, the only plugins associated with this manager should now
       be dynamic and unloaded. */
    auto ait = _plugins.aliases.cbegin();
    while(ait != _plugins.aliases.cend()) {
        if(ait->second.manager == this && ait->second.loadState != LoadState::Static)
            ait = _plugins.aliases.erase(ait);
        else ++ait;
    }

    #if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
    /* Remove the plugins from global container */
    for(auto it = removed.cbegin(); it != removed.cend(); ++it) {
        delete (*it)->second;
        _plugins.plugins.erase(*it);
    }
    #endif
}

#if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
LoadState AbstractManager::unloadRecursive(const std::string& plugin) {
    const auto found = _plugins.plugins.find(plugin);
    CORRADE_INTERNAL_ASSERT(found != _plugins.plugins.end());
    return unloadRecursiveInternal(*found->second);
}

LoadState AbstractManager::unloadRecursiveInternal(Plugin& plugin) {
    /* Plugin doesn't belong to this manager, cannot do anything (will assert
       on unload() in parent call) */
    if(plugin.manager != this) return LoadState::NotFound;

    /* If the plugin is not static and is used by others, try to unload these
       first so it can be unloaded too */
    if(plugin.loadState != LoadState::Static)
        for(auto it = plugin.metadata._usedBy.begin(); it != plugin.metadata._usedBy.end(); ++it)
            unloadRecursive(*it);

    /* Unload the plugin */
    /* GCC 4.5 requires explicit type */
    const LoadState after = unloadInternal(plugin);
    CORRADE_ASSERT(after & (LoadState::Static|LoadState::NotLoaded|LoadState::WrongMetadataFile),
        "PluginManager::Manager: cannot unload plugin" << plugin.metadata._name << "on manager destruction:" << after, LoadState{});

    return after;
}

std::string AbstractManager::pluginDirectory() const {
    return _pluginDirectory;
}

void AbstractManager::setPluginDirectory(std::string directory) {
    _pluginDirectory = std::move(directory);

    /* Remove aliases for unloaded plugins from the container. They need to be
       removed before plugins themselves */
    auto ait = _plugins.aliases.cbegin();
    while(ait != _plugins.aliases.cend()) {
        if(ait->second.manager == this && ait->second.loadState & (LoadState::NotLoaded|LoadState::WrongMetadataFile))
            ait = _plugins.aliases.erase(ait);
        else ++ait;
    }

    /* Remove all unloaded plugins from the container */
    auto it = _plugins.plugins.cbegin();
    while(it != _plugins.plugins.cend()) {
        if(it->second->manager == this && it->second->loadState & (LoadState::NotLoaded|LoadState::WrongMetadataFile)) {
            delete it->second;

            #ifndef CORRADE_GCC44_COMPATIBILITY
            it = _plugins.plugins.erase(it);
            #else
            /* GCC 4.4 returns void from map::erase(), but other iterators
               aren't invalidated, so it's safe */
            auto erase = it;
            ++it;
            plugins()->erase(erase);
            #endif

        } else ++it;
    }

    /* Find plugin files in the directory */
    static const std::size_t suffixSize = std::strlen(PLUGIN_FILENAME_SUFFIX);
    const std::vector<std::string> d = Directory::list(_pluginDirectory,
        Directory::Flag::SkipDirectories|Directory::Flag::SkipDotAndDotDot);
    for(auto it = d.cbegin(); it != d.cend(); ++it) {
        const std::string& filename = *it;
        /* File doesn't have module suffix, continue to next */
        const std::size_t end = filename.length()-suffixSize;
        if(filename.substr(end) != PLUGIN_FILENAME_SUFFIX)
            continue;

        /* Dig plugin name from filename */
        const std::string name = filename.substr(0, end);

        /* Skip the plugin if it is among loaded */
        if(_plugins.plugins.find(name) != _plugins.plugins.end()) continue;

        /* Insert plugin to list */
        const auto result = _plugins.plugins.insert({name, new Plugin(name, Directory::join(_pluginDirectory, name + ".conf"), this)});
        CORRADE_INTERNAL_ASSERT(result.second);

        /* Add aliases to the list */
        for(auto it = result.first->second->metadata._provides.begin(); it != result.first->second->metadata._provides.end(); ++it) {
            #ifndef CORRADE_GCC47_COMPATIBILITY
            _plugins.aliases.emplace(*it, *result.first->second);
            #else
            _plugins.aliases.insert({*it, *result.first->second});
            #endif
        }
    }
}

void AbstractManager::reloadPluginDirectory() {
    setPluginDirectory(pluginDirectory());
}
#endif

std::vector<std::string> AbstractManager::pluginList() const {
    std::vector<std::string> names;
    for(auto i = _plugins.plugins.cbegin(); i != _plugins.plugins.cend(); ++i) {

        /* Plugin doesn't belong to this manager */
        if(i->second->manager != this) continue;

        names.push_back(i->first);
    }
    return names;
}

auto AbstractManager::findWithAlias(const std::string& plugin) -> Plugin* {
    return const_cast<Plugin*>(const_cast<const AbstractManager&>(*this).findWithAlias(plugin));
}

auto AbstractManager::findWithAlias(const std::string& plugin) const -> const Plugin* {
    const auto found = _plugins.plugins.find(plugin);

    /* Not found, try aliases */
    if(found == _plugins.plugins.end()) {
        const auto aliasFound = _plugins.aliases.find(plugin);

        /* Found alias which belongs to this manager, load */
        if(aliasFound != _plugins.aliases.end() && aliasFound->second.manager == this)
            return &aliasFound->second;

    /* Found and belongs to this manager, load */
    } else if(found->second->manager == this) return found->second;

    /* Not found */
    return nullptr;
}

const PluginMetadata* AbstractManager::metadata(const std::string& plugin) const {
    if(const Plugin* const found = findWithAlias(plugin)) return &found->metadata;

    return nullptr;
}

LoadState AbstractManager::loadState(const std::string& plugin) const {
    if(const Plugin* const found = findWithAlias(plugin))
        return found->loadState;

    return LoadState::NotFound;
}

LoadState AbstractManager::load(const std::string& plugin) {
    if(Plugin* const found = findWithAlias(plugin)) {
        #if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
        return loadInternal(*found);
        #else
        return found->loadState;
        #endif
    }

    Error() << "PluginManager::Manager::load(): plugin" << plugin
        #if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
        << "is not static and was not found in" << _pluginDirectory;
        #else
        << "was not found";
        #endif
    return LoadState::NotFound;
}

#if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
LoadState AbstractManager::loadInternal(Plugin& plugin) {
    /* Plugin is not ready to load */
    if(plugin.loadState != LoadState::NotLoaded) {
        if(!(plugin.loadState & (LoadState::Static|LoadState::Loaded)))
            Error() << "PluginManager::Manager::load(): plugin" << plugin.metadata._name << "is not ready to load:" << plugin.loadState;
        return plugin.loadState;
    }

    /* Load dependencies and remember their names for later. Their names will
       be added to usedBy list only if everything goes well. */
    std::vector<std::reference_wrapper<Plugin>> dependencies;
    dependencies.reserve(plugin.metadata._depends.size());
    for(auto it = plugin.metadata._depends.begin(); it != plugin.metadata._depends.end(); ++it) {
        /* Find manager which is associated to this plugin and load the plugin
           with it */
        const auto dependency = _plugins.plugins.find(*it);

        if(dependency == _plugins.plugins.end() || !dependency->second->manager ||
           !(dependency->second->manager->loadInternal(*dependency->second) & LoadState::Loaded))
        {
            Error() << "PluginManager::Manager::load(): unresolved dependency" << *it << "of plugin" << plugin.metadata._name;
            return LoadState::UnresolvedDependency;
        }

        dependencies.push_back(*dependency->second);
    }

    const std::string filename = Directory::join(_pluginDirectory, plugin.metadata._name + PLUGIN_FILENAME_SUFFIX);

    /* Open plugin file, make symbols available for next libs (which depends on this) */
    #ifndef CORRADE_TARGET_WINDOWS
    void* module = dlopen(filename.data(), RTLD_NOW|RTLD_GLOBAL);
    #else
    HMODULE module = LoadLibraryA(filename.data());
    #endif
    if(!module) {
        Error() << "PluginManager::Manager::load(): cannot open plugin file" << '"' + filename + "\":" << dlerror();
        return LoadState::LoadFailed;
    }

    /* Check plugin version */
    #ifdef __GNUC__ /* http://www.mr-edd.co.uk/blog/supressing_gcc_warnings */
    __extension__
    #endif
    int (*_version)(void) = reinterpret_cast<int(*)()>(dlsym(module, "pluginVersion"));
    if(_version == nullptr) {
        Error() << "PluginManager::Manager::load(): cannot get version of plugin" << plugin.metadata._name + ":" << dlerror();
        dlclose(module);
        return LoadState::LoadFailed;
    }
    if(_version() != Version) {
        Error() << "PluginManager::Manager::load(): wrong version of plugin" << plugin.metadata._name + ", expected" << Version << "but got" << _version();
        dlclose(module);
        return LoadState::WrongPluginVersion;
    }

    /* Check interface string */
    #ifdef __GNUC__ /* http://www.mr-edd.co.uk/blog/supressing_gcc_warnings */
    __extension__
    #endif
    const char* (*interface)() = reinterpret_cast<const char* (*)()>(dlsym(module, "pluginInterface"));
    if(interface == nullptr) {
        Error() << "PluginManager::Manager::load(): cannot get interface string of plugin" << plugin.metadata._name + ":" << dlerror();
        dlclose(module);
        return LoadState::LoadFailed;
    }
    if(interface() != pluginInterface()) {
        Error() << "PluginManager::Manager::load(): wrong interface string of plugin" << plugin.metadata._name + ", expected" << pluginInterface() << "but got" << interface();
        dlclose(module);
        return LoadState::WrongInterfaceVersion;
    }

    /* Load plugin instancer */
    #ifdef __GNUC__ /* http://www.mr-edd.co.uk/blog/supressing_gcc_warnings */
    __extension__
    #endif
    Instancer instancer = reinterpret_cast<Instancer>(dlsym(module, "pluginInstancer"));
    if(instancer == nullptr) {
        Error() << "PluginManager::Manager::load(): cannot get instancer of plugin" << plugin.metadata._name + ":" << dlerror();
        dlclose(module);
        return LoadState::LoadFailed;
    }

    /* Initialize plugin */
    #ifdef __GNUC__ /* http://www.mr-edd.co.uk/blog/supressing_gcc_warnings */
    __extension__
    #endif
    void(*initializer)() = reinterpret_cast<void(*)()>(dlsym(module, "pluginInitializer"));
    if(initializer == nullptr) {
        Error() << "PluginManager::Manager::load(): cannot get initializer of plugin" << plugin.metadata._name + ":" << dlerror();
        dlclose(module);
        return LoadState::LoadFailed;
    }
    initializer();

    /* Everything is okay, add this plugin to usedBy list of each dependency */
    for(auto it = dependencies.begin(); it != dependencies.end(); ++it) {
        Plugin& dependency = *it;

        /* If the plugin is not static with no associated manager, use its
           manager for adding this plugin */
        if(dependency.manager)
            dependency.manager->addUsedBy(dependency.metadata._name, plugin.metadata._name);

        /* Otherwise add this plugin manually */
        else dependency.metadata._usedBy.push_back(plugin.metadata._name);
    }

    /* Update plugin object, set state to loaded */
    plugin.loadState = LoadState::Loaded;
    plugin.module = module;
    plugin.instancer = instancer;
    return LoadState::Loaded;
}
#endif

LoadState AbstractManager::unload(const std::string& plugin) {
    if(Plugin* const found = findWithAlias(plugin)) {
        #if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
        return unloadInternal(*found);
        #else
        return found->loadState;
        #endif
    }

    Error() << "PluginManager::Manager::unload(): plugin" << plugin << "was not found";
    return LoadState::NotFound;
}

#if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
LoadState AbstractManager::unloadInternal(Plugin& plugin) {
    /* Plugin is not ready to unload, nothing to do */
    if(plugin.loadState != LoadState::Loaded) {
        if(!(plugin.loadState & (LoadState::Static|LoadState::NotLoaded|LoadState::WrongMetadataFile)))
            Error() << "PluginManager::Manager::unload(): plugin" << plugin.metadata._name << "is not ready to unload:" << plugin.loadState;
        return plugin.loadState;
    }

    /* Plugin is used by another plugin, don't unload */
    if(!plugin.metadata._usedBy.empty()) {
        Error() << "PluginManager::Manager::unload(): plugin" << plugin.metadata._name << "is required by other plugins:" << plugin.metadata._usedBy;
        return LoadState::Required;
    }

    /* Plugin has active instances */
    auto foundInstance = instances.find(plugin.metadata._name);
    if(foundInstance != instances.end()) {
        /* Check if all instances can be safely deleted */
        for(auto it = foundInstance->second.cbegin(); it != foundInstance->second.cend(); ++it)
            if(!(*it)->canBeDeleted()) {
                Error() << "PluginManager::Manager::unload(): plugin" << plugin.metadata._name << "is currently used and cannot be deleted";
                return LoadState::Used;
            }

        /* If they can be, delete them. They remove itself from instances
           list on destruction, thus going backwards */
        for(std::size_t i = foundInstance->second.size(); i != 0; --i)
            delete foundInstance->second[i-1];
    }

    /* Remove this plugin from "used by" list of dependencies */
    for(auto it = plugin.metadata.depends().cbegin(); it != plugin.metadata.depends().cend(); ++it) {
        auto mit = _plugins.plugins.find(*it);

        if(mit != _plugins.plugins.end()) {
            /* If the plugin is not static with no associated manager, use
               its manager for removing this plugin */
            if(mit->second->manager)
                mit->second->manager->removeUsedBy(mit->first, plugin.metadata._name);

            /* Otherwise remove this plugin manually */
            else for(auto it = mit->second->metadata._usedBy.begin(); it != mit->second->metadata._usedBy.end(); ++it) {
                if(*it == plugin.metadata._name) {
                    mit->second->metadata._usedBy.erase(it);
                    break;
                }
            }
        }
    }

    /* Finalize plugin */
    #ifdef __GNUC__ /* http://www.mr-edd.co.uk/blog/supressing_gcc_warnings */
    __extension__
    #endif
    void(*finalizer)() = reinterpret_cast<void(*)()>(dlsym(plugin.module, "pluginFinalizer"));
    if(finalizer == nullptr) {
        Error() << "PluginManager::Manager::unload(): cannot get finalizer of plugin" << plugin.metadata._name + ":" << dlerror();
        /* Not fatal, continue with unloading */
    }
    finalizer();

    /* Close the module */
    #ifndef CORRADE_TARGET_WINDOWS
    if(dlclose(plugin.module) != 0) {
    #else
    if(!FreeLibrary(pluginObject.module)) {
    #endif
        Error() << "PluginManager::Manager::unload(): cannot unload plugin" << plugin.metadata._name + ":" << dlerror();
        plugin.loadState = LoadState::NotLoaded;
        return LoadState::UnloadFailed;
    }

    /* Update plugin object, set state to not loaded */
    plugin.loadState = LoadState::NotLoaded;
    plugin.module = nullptr;
    plugin.instancer = nullptr;
    return LoadState::NotLoaded;
}
#endif

void AbstractManager::registerInstance(std::string plugin, AbstractPlugin& instance, const PluginMetadata*& metadata) {
    /** @todo assert proper interface */
    auto foundPlugin = _plugins.plugins.find(plugin);

    CORRADE_ASSERT(foundPlugin != _plugins.plugins.end() && foundPlugin->second->manager == this,
        "PluginManager::AbstractPlugin::AbstractPlugin(): attempt to register instance of plugin not known to given manager", );

    auto foundInstance = instances.find(plugin);

    if(foundInstance == instances.end())
        foundInstance = instances.insert({std::move(plugin), {}}).first;

    foundInstance->second.push_back(&instance);

    metadata = &foundPlugin->second->metadata;
}

void AbstractManager::unregisterInstance(const std::string& plugin, AbstractPlugin& instance) {
    auto foundPlugin = _plugins.plugins.find(plugin);

    CORRADE_INTERNAL_ASSERT(foundPlugin != _plugins.plugins.end() && foundPlugin->second->manager == this);

    auto foundInstance = instances.find(plugin);
    CORRADE_INTERNAL_ASSERT(foundInstance != instances.end());
    std::vector<AbstractPlugin*>& _instances = foundInstance->second;

    auto pos = std::find(_instances.begin(), _instances.end(), &instance);
    CORRADE_INTERNAL_ASSERT(pos != _instances.end());

    _instances.erase(pos);

    if(_instances.empty()) instances.erase(plugin);
}

void AbstractManager::addUsedBy(const std::string& plugin, std::string usedBy) {
    auto found = _plugins.plugins.find(plugin);

    CORRADE_INTERNAL_ASSERT(found != _plugins.plugins.end());

    found->second->metadata._usedBy.push_back(std::move(usedBy));
}

void AbstractManager::removeUsedBy(const std::string& plugin, const std::string& usedBy) {
    auto found = _plugins.plugins.find(plugin);

    CORRADE_INTERNAL_ASSERT(found != _plugins.plugins.end());

    for(auto it = found->second->metadata._usedBy.begin(); it != found->second->metadata._usedBy.end(); ++it) {
        if(*it == usedBy) {
            found->second->metadata._usedBy.erase(it);
            return;
        }
    }
}

void* AbstractManager::instanceInternal(const std::string& plugin) {
    Plugin* const found = findWithAlias(plugin);

    CORRADE_ASSERT(found && (found->loadState & LoadState::Loaded),
        "PluginManager::Manager::instance(): plugin" << plugin << "is not loaded", nullptr);

    /* Instance the plugin using its original (non-aliased) name */
    return found->instancer(*this, found->metadata._name);
}

#if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
AbstractManager::Plugin::Plugin(std::string name, const std::string& metadata, AbstractManager* manager): configuration{metadata, Utility::Configuration::Flag::ReadOnly}, metadata{std::move(name), configuration}, manager{manager}, instancer{nullptr}, module{nullptr} {
    loadState = configuration.isValid() ? LoadState::NotLoaded : LoadState::WrongMetadataFile;
}
#endif

AbstractManager::Plugin::Plugin(std::string name, std::istream& metadata, StaticPlugin* staticPlugin): loadState{LoadState::Static}, configuration{metadata, Utility::Configuration::Flag::ReadOnly}, metadata{std::move(name), configuration}, manager{nullptr}, instancer{staticPlugin->instancer}, staticPlugin{staticPlugin} {}

AbstractManager::Plugin::~Plugin() {
    #ifndef CORRADE_TARGET_NACL_NEWLIB
    if(loadState == LoadState::Static)
    #endif
        delete staticPlugin;
}

#ifndef DOXYGEN_GENERATING_OUTPUT
Utility::Debug operator<<(Utility::Debug debug, PluginManager::LoadState value) {
    switch(value) {
        #define ls(state) case PluginManager::LoadState::state: return debug << "PluginManager::LoadState::" #state;
        ls(NotFound)
        #if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
        ls(WrongPluginVersion)
        ls(WrongInterfaceVersion)
        ls(WrongMetadataFile)
        ls(UnresolvedDependency)
        ls(LoadFailed)
        ls(Loaded)
        ls(NotLoaded)
        ls(UnloadFailed)
        ls(Required)
        #endif
        ls(Static)
        #if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(CORRADE_TARGET_EMSCRIPTEN)
        ls(Used)
        #endif
        #undef ls
    }

    return debug << "PluginManager::LoadState::(invalid)";
}
#endif

}}
