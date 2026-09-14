# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

project(linux-ws-audio-capture
  VERSION 1.0.0
  DESCRIPTION "Real-time desktop audio capture exposed as a C ABI"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# libpulse-simple is the backend; it carries libpulse and pthread along with it.
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple)

set(WSAUDIO_SOURCES
  src/capture.cpp
  src/log.cpp
  src/pulse_control.cpp
  src/pulse_mic.cpp)

add_library(wsaudio SHARED ${WSAUDIO_SOURCES})

target_include_directories(wsaudio
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(wsaudio PRIVATE PkgConfig::PULSE Threads::Threads)

target_compile_options(wsaudio PRIVATE -Wall -Wextra -Wpedantic)

# Only the wsa_* entry points are part of the ABI; everything else is hidden.
set_target_properties(wsaudio PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# Small CLI that dumps captured audio to a WAV file. Written in C on purpose:
# it is the proof that the ABI is callable from plain C.
add_executable(wsacap-dump examples/wsacap-dump.c)
target_link_libraries(wsacap-dump PRIVATE wsaudio)

# Self test. Needs a running audio server, so it is registered as a test but is
# expected to be run on a live session.
add_executable(wsaudio_selftest tests/wsaudio_selftest.cpp)
target_link_libraries(wsaudio_selftest PRIVATE wsaudio)

enable_testing()
add_test(NAME selftest COMMAND wsaudio_selftest)

include(GNUInstallDirs)

configure_file(cmake/wsacapture.pc.in wsacapture.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/wsacapture.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

install(TARGETS wsaudio
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/wsacapture.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS wsacap-dump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
