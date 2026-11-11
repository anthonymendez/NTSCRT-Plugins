cmake_minimum_required(VERSION 3.24)
# The version lives in VERSION (one line, X.Y.Z or X.Y.Z-rc.N): see CHANGELOG.md for how it moves.
# project() only takes the numeric X.Y.Z part (PROJECT_VERSION); NTSCRT_VERSION keeps the -rc.N.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NTSCRT_VERSION LIMIT_COUNT 1)
string(STRIP "${NTSCRT_VERSION}" NTSCRT_VERSION)
if(NOT NTSCRT_VERSION MATCHES "^([0-9]+[.][0-9]+[.][0-9]+)(-rc[.][0-9]+)?$")
  message(FATAL_ERROR "VERSION must be X.Y.Z or X.Y.Z-rc.N, got '${NTSCRT_VERSION}'")
endif()
set(NTSCRT_VERSION_CORE "${CMAKE_MATCH_1}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
# Must be set before project(): project() creates the cache entry (empty) otherwise, and the cargo
# builds pass it on as MACOSX_DEPLOYMENT_TARGET (rustc rejects an empty value).
if(CMAKE_HOST_APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()
project(NTSCRTPlugins VERSION ${NTSCRT_VERSION_CORE} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(APPLE)
  enable_language(OBJCXX)
endif()
if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive- /Zc:__cplusplus)
  add_compile_definitions(_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING _CRT_SECURE_NO_WARNINGS NOMINMAX)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: plugin DLL has no runtime deps
endif()

set(NTSCRT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND CMAKE_MODULE_PATH ${NTSCRT_ROOT}/cmake)
include(Embed)
include(Shaders)
include(Deps)

option(NTSCRT_BUILD_OFX "Build the OpenFX plugin (Resolve)" ON)
option(NTSCRT_BUILD_TOOLS "Build ntscrt-cli" ON)
option(NTSCRT_BUILD_TESTS "Build tests" ON)

add_subdirectory(core)
add_subdirectory(hostparams)  # shared host param model (OpenFX + FxPlug)
if(NTSCRT_BUILD_TOOLS)
  add_subdirectory(tools/ntscrt-cli)
endif()
if(NTSCRT_BUILD_OFX AND EXISTS ${NTSCRT_ROOT}/ofx/CMakeLists.txt)
  add_subdirectory(ofx)
endif()
if(NTSCRT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
# FxPlug 4 / Final Cut Pro (agent F): portable param model + its test on every
# platform; the plug-in bundle itself only on macOS with the FxPlug SDK installed.
if(EXISTS ${NTSCRT_ROOT}/fxplug/CMakeLists.txt)
  add_subdirectory(fxplug)
endif()
