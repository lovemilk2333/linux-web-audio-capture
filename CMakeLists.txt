# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

project(linux-web-audio-capture
  VERSION 1.0.0
  DESCRIPTION "Real-time desktop audio capture exposed as a C ABI"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Record the build tree's rpath relative to $ORIGIN. Without this CMake bakes in
# absolute paths, so moving or renaming the checkout leaves the test binaries
# unable to find libwebaudio.so until they are rebuilt.
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# libpulse-simple is the backend; it carries libpulse and pthread along with it.
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple)

set(WEBAUDIO_SOURCES
  src/capture.cpp
  src/log.cpp
  src/pulse_control.cpp
  src/pulse_mic.cpp)

add_library(webaudio SHARED ${WEBAUDIO_SOURCES})

target_include_directories(webaudio
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(webaudio PRIVATE PkgConfig::PULSE Threads::Threads)

target_compile_options(webaudio PRIVATE -Wall -Wextra -Wpedantic)

# Only the weba_* entry points are part of the ABI; everything else is hidden.
set_target_properties(webaudio PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# Small CLI that dumps captured audio to a WAV file. Written in C on purpose:
# it is the proof that the ABI is callable from plain C.
add_executable(webacap-dump examples/webacap-dump.c)
target_link_libraries(webacap-dump PRIVATE webaudio)

# Self test. Needs a running audio server, so it is registered as a test but is
# expected to be run on a live session.
add_executable(webaudio_selftest tests/webaudio_selftest.cpp)
target_link_libraries(webaudio_selftest PRIVATE webaudio)

enable_testing()
add_test(NAME selftest COMMAND webaudio_selftest)

include(GNUInstallDirs)

# Depth from the pkgconfig directory back up to the install prefix, so the
# generated .pc can be relocatable rather than naming a configure-time path.
file(RELATIVE_PATH PC_REL_PREFIX
  "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
string(REGEX REPLACE "/$" "" PC_REL_PREFIX "${PC_REL_PREFIX}")

configure_file(cmake/webacapture.pc.in webacapture.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/webacapture.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

install(TARGETS webaudio
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/webacapture.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS webacap-dump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
