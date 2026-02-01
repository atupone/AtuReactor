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
set(ATU_REACTOR_VERSION 0.0.11)
set(ATU_REACTOR_SOVERSION 0)

# ---------------------------------------------------------------------------
# SYSTEM PATHS & INSTALLATION LOGIC
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 1. Generate the Config file from the template
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AtuReactorConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/AtuReactorConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AtuReactor"
)

# ---------------------------------------------------------------------------
# COMPILER SETTINGS
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STRICT_WERROR "Treat warnings as errors" OFF)

# Add common compiler flags for safety and debugging
add_compile_options(
    -Wall -Wextra
    -Wconversion
    -Wshadow
    -Wnon-virtual-dtor
    -ggdb -O2
)

if(STRICT_WERROR)
    add_compile_options(-Werror)
endif()

# ---------------------------------------------------------------------------
# THE LIBRARY
# ---------------------------------------------------------------------------
add_library(AtuReactor SHARED
    src/EventLoop.cc
    src/PacketReceiver.cc
    src/UDPReceiver.cc
    src/PcapReceiver.cc
)

add_library(AtuReactor::AtuReactor ALIAS AtuReactor)

# Define include directories for the library
target_include_directories(AtuReactor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

set_target_properties(AtuReactor PROPERTIES
    VERSION ${ATU_REACTOR_VERSION}
    SOVERSION ${ATU_REACTOR_SOVERSION}
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(AtuReactor PUBLIC pcap)
# ---------------------------------------------------------------------------
# INSTALLATION & PACKAGE CONFIGURATION
# ---------------------------------------------------------------------------

#  Install the Library (the .so files)
install(TARGETS AtuReactor
    EXPORT AtuReactorTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 2. Install the Public Headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/atu_reactor
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# Create a basic Config file so find_package(AtuReactor) works
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AtuReactorConfigVersion.cmake"
    VERSION ${ATU_REACTOR_VERSION}
    COMPATIBILITY AnyNewerVersion
)

# Install the export set
install(EXPORT AtuReactorTargets
    FILE AtuReactorTargets.cmake
    NAMESPACE AtuReactor::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AtuReactor
)

# Install the Config and Version files
install(
    FILES
    "${CMAKE_CURRENT_BINARY_DIR}/AtuReactorConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AtuReactorConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AtuReactor
)

# Install Examples (Optional documentation)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/examples/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples
    FILES_MATCHING PATTERN "*.cc"
)


# ---------------------------------------------------------------------------
# EXAMPLES
# ---------------------------------------------------------------------------
option(BUILD_EXAMPLES "Build the example programs" OFF)

if(BUILD_EXAMPLES)
    # 1. Simple Echo Server
    add_executable(example_echo examples/simple_echo.cc)
    target_link_libraries(example_echo PRIVATE AtuReactor)

    # 2. Multi-port Gateway
    add_executable(example_gateway examples/multi_port_gateway.cc)
    target_link_libraries(example_gateway PRIVATE AtuReactor)

    # 3. Timer Usage
    add_executable(example_timers examples/TimerUsage.cc)
    target_link_libraries(example_timers PRIVATE AtuReactor)

    # 4. pcap replay
    add_executable(pcap_replay.cc examples/pcap_replay.cc)
    target_link_libraries(pcap_replay.cc PRIVATE AtuReactor)

    message(STATUS "Example programs are enabled")
else()
    message(STATUS "Example programs are disabled (Use -DBUILD_EXAMPLES=ON to enable)")
endif()

# ---------------------------------------------------------------------------
# TEST SUITE: Find or Download GTest
# ---------------------------------------------------------------------------

# Add an option to toggle tests (Default is OFF)
option(BUILD_TESTING "Build the unit tests" OFF)

if(BUILD_TESTING)
    enable_testing()

    find_package(GTest QUIET)

    if(NOT GTest_FOUND)
        message(STATUS "GTest not found. Downloading via FetchContent...")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/heads/main.zip
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)

        if(NOT TARGET GTest::GTest)
            add_library(GTest::GTest ALIAS gtest)
            add_library(GTest::Main ALIAS gtest_main)
        endif()
    endif()

    # UDP Receiver Test
    add_executable(AtuReactorTests tests/UDPReceiverTest.cc)
    target_link_libraries(AtuReactorTests PRIVATE AtuReactor GTest::GTest GTest::Main)
    add_test(NAME UDPReceiverTest COMMAND AtuReactorTests)

    # Timer Tests
    add_executable(TimerTests tests/TimerTest.cc)
    target_link_libraries(TimerTests PRIVATE AtuReactor GTest::GTest GTest::Main)
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
