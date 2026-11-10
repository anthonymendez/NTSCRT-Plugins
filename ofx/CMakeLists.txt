# NTSCRT OpenFX plugin (DaVinci Resolve 21.1.2+, Windows x64 / macOS universal).
#
# Output: <build>/ofx/<Config>/NTSCRT.ofx.bundle/
#   Contents/Win64/NTSCRT.ofx          (Windows DLL, static CRT)
#   Contents/MacOS/NTSCRT.ofx          (macOS; universal when configured with
#                                       -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64")
#   Contents/Info.plist                (macOS)
#   Contents/Resources/presets/        (copy of repo presets/)
#   Contents/Resources/shaders/        (copy of resources/shaders if it exists)
#   Contents/Resources/<librashader>   (NTSCRT_LIBRASHADER_LIB, if set)
include(FetchContent)

# OpenFX SDK headers + C++ Support library. SOURCE_SUBDIR points nowhere so the
# SDK's own CMake project (conan-based) is not added; we build Support ourselves.
FetchContent_Declare(openfx
  GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openfx.git
  GIT_TAG OFX_Release_1.5.1
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_MakeAvailable(openfx)

add_library(ntscrt_ofx_headers INTERFACE)
target_include_directories(ntscrt_ofx_headers INTERFACE ${openfx_SOURCE_DIR}/include)

set(OFXS ${openfx_SOURCE_DIR}/Support/Library)
add_library(ntscrt_ofx_support STATIC
  ${OFXS}/ofxsCore.cpp
  ${OFXS}/ofxsImageEffect.cpp
  ${OFXS}/ofxsInteract.cpp
  ${OFXS}/ofxsLog.cpp
  ${OFXS}/ofxsMultiThread.cpp
  ${OFXS}/ofxsParams.cpp
  ${OFXS}/ofxsProperty.cpp
  ${OFXS}/ofxsPropertyValidation.cpp)
target_include_directories(ntscrt_ofx_support PUBLIC ${openfx_SOURCE_DIR}/Support/include)
target_link_libraries(ntscrt_ofx_support PUBLIC ntscrt_ofx_headers)
if(MSVC)
  target_compile_options(ntscrt_ofx_support PRIVATE /W0 /permissive)
else()
  target_compile_options(ntscrt_ofx_support PRIVATE -w)
endif()

add_library(ntscrt_ofx MODULE
  ntscrt_ofx.cpp
  ofx_params.cpp
  ofx_params.h)
target_link_libraries(ntscrt_ofx PRIVATE ntscrt_ofx_support ntscrt_core nlohmann_json::nlohmann_json)
target_compile_definitions(ntscrt_ofx PRIVATE
  NTSCRT_OFX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  NTSCRT_OFX_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  NTSCRT_OFX_VERSION_PATCH=${PROJECT_VERSION_PATCH})

if(WIN32)
  set(NTSCRT_OFX_ARCHDIR Win64)
elseif(APPLE)
  set(NTSCRT_OFX_ARCHDIR MacOS)
else()
  set(NTSCRT_OFX_ARCHDIR Linux-x86-64)
endif()
# $<CONFIG> is in the path explicitly so multi-config generators don't append
# another config directory below Contents/<arch>.
set(NTSCRT_OFX_BUNDLE ${CMAKE_BINARY_DIR}/ofx/$<CONFIG>/NTSCRT.ofx.bundle)
set_target_properties(ntscrt_ofx PROPERTIES
  OUTPUT_NAME NTSCRT
  PREFIX ""
  SUFFIX ".ofx"
  LIBRARY_OUTPUT_DIRECTORY ${NTSCRT_OFX_BUNDLE}/Contents/${NTSCRT_OFX_ARCHDIR}
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/ofx/$<CONFIG>/lib
  PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/ofx/$<CONFIG>/pdb)
if(APPLE)
  # Plain Mach-O bundle (not a CFBundle target: OFX wants the .ofx file itself
  # inside Contents/MacOS, with the Info.plist we write below).
  # OfxGetPlugin/OfxGetNumberOfPlugins carry default visibility (Support lib EXPORT).
  set_target_properties(ntscrt_ofx PROPERTIES BUNDLE FALSE)
endif()

set(NTSCRT_LIBRASHADER_LIB "" CACHE FILEPATH "Optional librashader .dll/.dylib to copy into the OFX bundle Resources")
configure_file(Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
add_custom_command(TARGET ntscrt_ofx POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    -DBUNDLE=${NTSCRT_OFX_BUNDLE}
    -DPRESETS=${NTSCRT_ROOT}/presets
    -DSHADERS=${NTSCRT_ROOT}/resources/shaders
    -DLIBRASHADER=${NTSCRT_LIBRASHADER_LIB}
    -DPLIST=$<IF:$<BOOL:${APPLE}>,${CMAKE_CURRENT_BINARY_DIR}/Info.plist,>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/bundle_resources.cmake
  COMMENT "Assembling NTSCRT.ofx.bundle resources"
  VERBATIM)
