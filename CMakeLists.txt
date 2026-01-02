# Copyright (C) 2026 Alfredo Tupone
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

cmake_minimum_required(VERSION 3.10)
project(AtuReactor LANGUAGES CXX)

# Define the version and compatibility version
set(ATU_REACTOR_VERSION 0.0.3)
set(ATU_REACTOR_SOVERSION 0)

# Force C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add common compiler flags for safety and debugging
add_compile_options(
    -Wall -Wextra -Werror
    -Wconversion
    -Wshadow
    -Wnon-virtual-dtor
    -ggdb -O2
)

# ---------------------------------------------------------------------------
# 1. THE LIBRARY: AtuReactor
# ---------------------------------------------------------------------------
add_library(AtuReactor SHARED src/EventLoop.cc src/UDPReceiver.cc)
add_library(AtuReactor::AtuReactor ALIAS AtuReactor)

set_target_properties(AtuReactor PROPERTIES
    VERSION ${ATU_REACTOR_VERSION}
    SOVERSION ${ATU_REACTOR_SOVERSION}
    POSITION_INDEPENDENT_CODE ON
)

# Define include directories for the library
target_include_directories(AtuReactor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Define the installation paths
include(GNUInstallDirs)

# 1. Install the Library (the .so files)
install(TARGETS AtuReactor
    EXPORT AtuReactorTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install the export set
install(EXPORT AtuReactorTargets
    FILE AtuReactorTargets.cmake
    NAMESPACE AtuReactor::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AtuReactor
)

# Create a basic Config file so find_package(AtuReactor) works
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AtuReactorConfigVersion.cmake"
    VERSION ${ATU_REACTOR_VERSION}
    COMPATIBILITY AnyNewerVersion
)

# Install the Config and Version files
install(FILES
    "cmake/AtuReactorConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AtuReactorConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AtuReactor
)

# 2. Install the Public Headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/atu_reactor
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/examples/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples
    FILES_MATCHING PATTERN "*.cc"
)

# ---------------------------------------------------------------------------
# TEST SUITE
# ---------------------------------------------------------------------------

# Add an option to toggle tests (Default is OFF)
option(BUILD_TESTING "Build the unit tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    find_package(GTest REQUIRED)

    # UDP Receiver Test
    add_executable(AtuReactorTests tests/UDPReceiverTest.cc)
    target_link_libraries(AtuReactorTests PRIVATE AtuReactor GTest::GTest GTest::Main)
    target_include_directories(AtuReactorTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME UDPReceiverTest COMMAND AtuReactorTests)

    # Timer Tests
    add_executable(TimerTests tests/TimerTest.cc)
    target_link_libraries(TimerTests PRIVATE AtuReactor GTest::GTest GTest::Main)
    target_include_directories(TimerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME TimerTests COMMAND TimerTests)

    message(STATUS "Unit tests are enabled")
else()
    message(STATUS "Unit tests are disabled (Use -DBUILD_TESTING=ON to enable)")
endif()

# Local Variables: ***
# tab-width: 4 ***
# indent-tabs-mode: nil ***
# End: ***
# ex: shiftwidth=4 tabstop=4
