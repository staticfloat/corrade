#
#   This file is part of Corrade.
#
#   Copyright © 2007, 2008, 2009, 2010, 2011, 2012, 2013, 2014, 2015
#             Vladimír Vondruš <mosra@centrum.cz>
#
#   Permission is hereby granted, free of charge, to any person obtaining a
#   copy of this software and associated documentation files (the "Software"),
#   to deal in the Software without restriction, including without limitation
#   the rights to use, copy, modify, merge, publish, distribute, sublicense,
#   and/or sell copies of the Software, and to permit persons to whom the
#   Software is furnished to do so, subject to the following conditions:
#
#   The above copyright notice and this permission notice shall be included
#   in all copies or substantial portions of the Software.
#
#   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
#   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
#   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
#   DEALINGS IN THE SOFTWARE.
#

set(CorradeTestSuite_SRCS
    Tester.cpp

    Compare/File.cpp
    Compare/FileToString.cpp
    Compare/FloatingPoint.cpp
    Compare/StringToFile.cpp)

set(CorradeTestSuite_HEADERS
    Comparator.h
    Tester.h
    TestSuite.h
    visibility.h)

# TestSuite library
add_library(CorradeTestSuite ${SHARED_OR_STATIC}
    ${CorradeTestSuite_SRCS}
    ${CorradeTestSuite_HEADERS})
set_target_properties(CorradeTestSuite PROPERTIES DEBUG_POSTFIX "-d")
if(NOT BUILD_STATIC)
    set_target_properties(CorradeTestSuite PROPERTIES VERSION ${CORRADE_LIBRARY_VERSION} SOVERSION ${CORRADE_LIBRARY_SOVERSION})
elseif(BUILD_STATIC_PIC)
    set_target_properties(CorradeTestSuite PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_link_libraries(CorradeTestSuite CorradeUtility)

install(TARGETS CorradeTestSuite
        RUNTIME DESTINATION ${CORRADE_BINARY_INSTALL_DIR}
        LIBRARY DESTINATION ${CORRADE_LIBRARY_INSTALL_DIR}
        ARCHIVE DESTINATION ${CORRADE_LIBRARY_INSTALL_DIR})
install(FILES ${CorradeTestSuite_HEADERS} DESTINATION ${CORRADE_INCLUDE_INSTALL_DIR}/TestSuite)

add_subdirectory(Compare)

if(BUILD_TESTS)
    add_subdirectory(Test)
endif()

# Corrade TestSuite library for superprojects
set(CORRADE_TESTSUITE_LIBRARY CorradeTestSuite CACHE INTERNAL "")
