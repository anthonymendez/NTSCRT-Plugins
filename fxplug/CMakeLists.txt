# FxPlug 4 plug-in for Final Cut Pro / Motion (owner: agent F).
#
# - ntscrt_fx_model: portable parameter model, built + tested on every platform.
# - On macOS with the FxPlug SDK installed: "NTSCRT.app" wrapper application
#   containing the "NTSCRT Renderer.pluginkit" XPC service (FxTileableEffect),
#   the FxPlug/PluginManager frameworks, bundled presets/shaders and the Motion
#   template. See fxplug/README.md.

add_library(ntscrt_fx_model STATIC model/param_model.cpp)
target_include_directories(ntscrt_fx_model PUBLIC model)
target_link_libraries(ntscrt_fx_model PUBLIC nlohmann_json::nlohmann_json)

if(NTSCRT_BUILD_TESTS AND COMMAND ntscrt_add_test)
  ntscrt_add_test(test_fxplug_model tests/test_param_model.cpp)
  target_link_libraries(test_fxplug_model PRIVATE ntscrt_fx_model)
endif()

if(NOT APPLE)
  return()
endif()

# ---------------------------------------------------------------------------
option(NTSCRT_BUILD_FXPLUG "Build the FxPlug 4 plug-in (needs Apple's FxPlug SDK)" ON)
set(NTSCRT_FXPLUG_SDK_DIR "/Library/Developer" CACHE PATH "FxPlug SDK install root (has Frameworks/FxPlug.framework)")
set(NTSCRT_FXPLUG_BUNDLE_ID "com.anthonymendez.NTSCRT" CACHE STRING "Wrapper app bundle id (plug-in is <id>.Renderer)")
set(NTSCRT_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity for the FxPlug bundle ('-' = ad-hoc, else e.g. 'Developer ID Application: ...')")
set(NTSCRT_SHADERS_DIR "${NTSCRT_RESOURCES_BUILD_DIR}/shaders" CACHE PATH "Directory copied to the plug-in's Resources/shaders (slang-shaders subset); empty = skip")
if(NOT NTSCRT_BUILD_FXPLUG)
  return()
endif()

# Compile against the sparse SDK when present, embed the runtime frameworks.
find_path(NTSCRT_FXPLUG_FRAMEWORK_DIR FxPlug.framework
  PATHS "${NTSCRT_FXPLUG_SDK_DIR}/SDKs/FxPlug.sdk/Library/Frameworks"
        "${NTSCRT_FXPLUG_SDK_DIR}/Frameworks" /Library/Frameworks
  NO_DEFAULT_PATH)
find_path(NTSCRT_FXPLUG_EMBED_DIR FxPlug.framework
  PATHS "${NTSCRT_FXPLUG_SDK_DIR}/Frameworks" "${NTSCRT_FXPLUG_FRAMEWORK_DIR}"
  NO_DEFAULT_PATH)
if(NOT NTSCRT_FXPLUG_FRAMEWORK_DIR OR NOT EXISTS "${NTSCRT_FXPLUG_EMBED_DIR}/PluginManager.framework")
  message(STATUS "FxPlug SDK not found under ${NTSCRT_FXPLUG_SDK_DIR} - skipping the Final Cut Pro plug-in "
                 "(install the FxPlug SDK from developer.apple.com, see fxplug/README.md)")
  return()
endif()
message(STATUS "FxPlug: compiling against ${NTSCRT_FXPLUG_FRAMEWORK_DIR}, embedding from ${NTSCRT_FXPLUG_EMBED_DIR}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(FX_APP_NAME "NTSCRT")
set(FX_PLUGIN_NAME "NTSCRT Renderer")
set(FX_PLUGIN_EXE "NTSCRTRenderer")
set(FX_PLUGIN_UUID "6E1C2A8B-7D44-4C1E-9B7A-3F5D2E8C1A90")   # NEVER change: projects + Motion templates reference it
set(FX_GROUP_UUID "B3A9F0D2-5C61-4E8F-A2D7-91C4E6B8F035")
set(FX_VERSION "${PROJECT_VERSION}")

# --- the renderer (XPC service executable) ---------------------------------
add_executable(ntscrt_fxplug_renderer
  src/renderer_main.mm
  src/NTSCRTPlugIn.mm
  src/MetalSupport.mm)
set_target_properties(ntscrt_fxplug_renderer PROPERTIES
  OUTPUT_NAME "${FX_PLUGIN_EXE}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stage"
  BUILD_RPATH "@loader_path/../Frameworks"
  INSTALL_RPATH "@loader_path/../Frameworks"
  BUILD_WITH_INSTALL_RPATH ON)
target_compile_options(ntscrt_fxplug_renderer PRIVATE -fobjc-arc "-F${NTSCRT_FXPLUG_FRAMEWORK_DIR}")
target_link_options(ntscrt_fxplug_renderer PRIVATE "-F${NTSCRT_FXPLUG_FRAMEWORK_DIR}")
target_link_libraries(ntscrt_fxplug_renderer PRIVATE
  ntscrt_core ntscrt_fx_model
  "-framework FxPlug" "-framework PluginManager"
  "-framework Foundation" "-framework AppKit" "-framework Metal" "-framework IOSurface"
  "-framework CoreMedia" "-framework CoreVideo" "-framework UniformTypeIdentifiers")

# --- the wrapper application --------------------------------------------
configure_file(Info-App.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info-App.plist" @ONLY)
add_executable(ntscrt_fxplug_app MACOSX_BUNDLE src/app_main.mm)
set_target_properties(ntscrt_fxplug_app PROPERTIES
  OUTPUT_NAME "${FX_APP_NAME}"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info-App.plist"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist")
target_compile_options(ntscrt_fxplug_app PRIVATE -fobjc-arc)
target_link_libraries(ntscrt_fxplug_app PRIVATE "-framework Foundation" "-framework AppKit")

# --- generated files: plug-in Info.plist, Motion template ------------------
configure_file(Info-Renderer.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info-Renderer.plist" @ONLY)
set(FX_TEMPLATE "${CMAKE_CURRENT_BINARY_DIR}/template/NTSCRT/NTSCRT.moef")
set(FX_TEMPLATE_OVERRIDE "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/motion/NTSCRT/NTSCRT.moef")  # Motion-authored template wins
  set(FX_TEMPLATE_OVERRIDE "${CMAKE_CURRENT_SOURCE_DIR}/motion/NTSCRT/NTSCRT.moef")
endif()
add_custom_command(OUTPUT "${FX_TEMPLATE}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_motion_template.py"
          --uuid "${FX_PLUGIN_UUID}" --name "NTSCRT" --out "${FX_TEMPLATE}"
  DEPENDS tools/make_motion_template.py
          $<$<BOOL:${FX_TEMPLATE_OVERRIDE}>:${FX_TEMPLATE_OVERRIDE}>
  COMMENT "Generating Motion template")

# --- assemble + sign the bundle ----------------------------------------------
set(FX_APP_BUNDLE "$<TARGET_BUNDLE_DIR:ntscrt_fxplug_app>")
add_custom_target(ntscrt_fxplug ALL
  COMMAND /bin/bash "${CMAKE_CURRENT_SOURCE_DIR}/cmake/assemble_bundle.sh"
    "--app=${FX_APP_BUNDLE}"
    "--renderer=$<TARGET_FILE:ntscrt_fxplug_renderer>"
    "--plugin-name=${FX_PLUGIN_NAME}"
    "--plist=${CMAKE_CURRENT_BINARY_DIR}/Info-Renderer.plist"
    "--frameworks=${NTSCRT_FXPLUG_EMBED_DIR}"
    "--presets=${NTSCRT_ROOT}/presets"
    "--shaders=${NTSCRT_SHADERS_DIR}"
    "--librashader=${NTSCRT_LIBRASHADER_BUILT}"
    "--param-ids=${CMAKE_CURRENT_SOURCE_DIR}/param_ids.json"
    "--license-root=${NTSCRT_ROOT}"
    "--template-dir=${CMAKE_CURRENT_BINARY_DIR}/template"
    "--renderer-entitlements=${CMAKE_CURRENT_SOURCE_DIR}/Renderer.entitlements"
    "--app-entitlements=${CMAKE_CURRENT_SOURCE_DIR}/App.entitlements"
    "--identity=${NTSCRT_CODESIGN_IDENTITY}"
  DEPENDS ntscrt_fxplug_renderer ntscrt_fxplug_app ntscrt_resources "${FX_TEMPLATE}"
          "${CMAKE_CURRENT_BINARY_DIR}/Info-Renderer.plist"
          cmake/assemble_bundle.sh Renderer.entitlements App.entitlements param_ids.json
          "${NTSCRT_ROOT}/LICENSE" "${NTSCRT_ROOT}/THIRD_PARTY_NOTICES.md"
  COMMENT "Assembling + signing ${FX_APP_NAME}.app (FxPlug)"
  VERBATIM)
