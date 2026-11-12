# Owner: agent D. CRT stage: librashader (runtime-loaded) + ShaderBypass.
#
# Also builds the runtime resources into ${NTSCRT_RESOURCES_BUILD_DIR}
# (default <build>/resources):
#   librashader.dll | librashader.dylib   cargo-built from vendor/librashader
#   shaders/...                           files of resources/shader-manifest.txt,
#                                         copied from vendor/slang-shaders
#   presets/...                           the bundled look presets
# Host packaging (OFX bundle, FxPlug) copies that directory into the plugin's
# resource dir; `install()` puts it under <prefix>/resources.
target_sources(ntscrt_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/crt.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/crt_libra.cpp)
ntscrt_add_shaders(ntscrt_core ${CMAKE_CURRENT_SOURCE_DIR}/shaders/crt_place.slang)

set(NTSCRT_LIBRASHADER_DIR ${NTSCRT_ROOT}/vendor/librashader CACHE PATH "librashader source (submodule, pinned 76462c03)")
set(NTSCRT_SLANG_SHADERS_DIR ${NTSCRT_ROOT}/vendor/slang-shaders CACHE PATH "libretro slang-shaders (submodule, pinned cb01f2f2)")
set(NTSCRT_LIBRASHADER_PREBUILT "" CACHE FILEPATH "Use this librashader dll/dylib instead of building it with cargo")
set(NTSCRT_RESOURCES_BUILD_DIR ${CMAKE_BINARY_DIR}/resources CACHE PATH "Where the runtime resources are assembled")
set(res ${NTSCRT_RESOURCES_BUILD_DIR})

if(NOT EXISTS ${NTSCRT_LIBRASHADER_DIR}/include/librashader.h)
  message(FATAL_ERROR "librashader headers not found in ${NTSCRT_LIBRASHADER_DIR}; run `git submodule update --init vendor/librashader`")
endif()
target_include_directories(ntscrt_core PRIVATE ${NTSCRT_LIBRASHADER_DIR}/include)

if(WIN32)
  target_sources(ntscrt_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/crt_libra_d3d11.cpp)
  set(lib_name librashader.dll)
elseif(APPLE)
  set(mm ${CMAKE_CURRENT_SOURCE_DIR}/crt_libra_metal.mm)
  target_sources(ntscrt_core PRIVATE ${mm})
  set_source_files_properties(${mm} TARGET_DIRECTORY ntscrt_core PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  set(lib_name librashader.dylib)
else()
  set(lib_name librashader.so)
endif()
if(NOT WIN32)
  target_link_libraries(ntscrt_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# ---- librashader (dynamic library, loaded at runtime; never linked, so its
# Rust std never meets ntsc-rs's static one) ----
set(lib_out ${res}/${lib_name})
# Each command that writes ${lib_out} makes its directory first: nothing else orders it before
# them (macOS CI: `lipo -create ... -output` ran before resources/ existed, intermittently).
get_filename_component(lib_out_dir ${lib_out} DIRECTORY)
set(NTSCRT_LIBRASHADER_BUILT ${lib_out} CACHE INTERNAL "librashader library assembled into the build resources dir")
if(NTSCRT_LIBRASHADER_PREBUILT)
  add_custom_command(OUTPUT ${lib_out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${lib_out_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NTSCRT_LIBRASHADER_PREBUILT} ${lib_out}
    DEPENDS ${NTSCRT_LIBRASHADER_PREBUILT}
    COMMENT "librashader (prebuilt) -> ${lib_out}")
else()
  find_program(NTSCRT_CARGO cargo HINTS $ENV{HOME}/.cargo/bin $ENV{USERPROFILE}/.cargo/bin)
  if(NOT NTSCRT_CARGO)
    message(FATAL_ERROR "cargo not found: install Rust or set NTSCRT_LIBRASHADER_PREBUILT")
  endif()
  set(cargo_dir ${CMAKE_BINARY_DIR}/librashader-target)
  # As upstream DEVELOPMENT.md: `cargo build --release -p librashader-capi --features stable`.
  # Only the runtime CrtStage uses: the default (all runtimes) makes librashader.dll import
  # D3DX9_43.dll (legacy DirectX 9 runtime, missing on a clean Windows: the DLL fails to load
  # with error 126), d3d12 and dxcompiler.
  if(WIN32)
    set(libra_features "stable,runtime-d3d11")
  elseif(APPLE)
    set(libra_features "stable,runtime-metal")
  else()
    set(libra_features "stable")
  endif()
  set(cargo_args build --release --locked -p librashader-capi --no-default-features --features ${libra_features})
  if(APPLE)
    # Universal dylib when building universal, else the host/target arch.
    set(archs ${CMAKE_OSX_ARCHITECTURES})
    if(NOT archs)
      set(archs ${CMAKE_HOST_SYSTEM_PROCESSOR})
    endif()
    set(cmds)
    set(slices)
    foreach(a ${archs})
      if(a STREQUAL arm64 OR a STREQUAL aarch64)
        set(triple aarch64-apple-darwin)
      else()
        set(triple x86_64-apple-darwin)
      endif()
      list(APPEND cmds COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${cargo_dir}
                           MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
                           ${NTSCRT_CARGO} ${cargo_args} --target ${triple})
      list(APPEND slices ${cargo_dir}/${triple}/release/liblibrashader_capi.dylib)
    endforeach()
    add_custom_command(OUTPUT ${lib_out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${lib_out_dir}
      ${cmds}
      COMMAND lipo -create ${slices} -output ${lib_out}
      COMMAND install_name_tool -id @rpath/librashader.dylib ${lib_out}
      WORKING_DIRECTORY ${NTSCRT_LIBRASHADER_DIR}
      DEPENDS ${NTSCRT_LIBRASHADER_DIR}/Cargo.lock
      COMMENT "cargo: librashader-capi (${archs}) -> ${lib_out}"
      VERBATIM)
  else()
    set(rustflags)
    if(WIN32)
      # Static CRT like the plugin itself: no vcruntime140.dll dependency.
      set(rustflags "RUSTFLAGS=-C target-feature=+crt-static")
      set(built ${cargo_dir}/release/librashader_capi.dll)
    else()
      set(built ${cargo_dir}/release/liblibrashader_capi.so)
    endif()
    add_custom_command(OUTPUT ${lib_out}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${lib_out_dir}
      COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${cargo_dir} ${rustflags} ${NTSCRT_CARGO} ${cargo_args}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${built} ${lib_out}
      WORKING_DIRECTORY ${NTSCRT_LIBRASHADER_DIR}
      DEPENDS ${NTSCRT_LIBRASHADER_DIR}/Cargo.lock
      COMMENT "cargo: librashader-capi -> ${lib_out}"
      VERBATIM)
  endif()
endif()

# ---- shader subset + presets ----
set(manifest ${NTSCRT_ROOT}/resources/shader-manifest.txt)
file(STRINGS ${manifest} shader_files REGEX "^[^#]")
set(shader_srcs)
foreach(f ${shader_files})
  list(APPEND shader_srcs ${NTSCRT_SLANG_SHADERS_DIR}/${f})
endforeach()
# CONFIGURE_DEPENDS: a preset added by an upstream sync is picked up without a manual re-configure.
file(GLOB_RECURSE preset_files CONFIGURE_DEPENDS ${NTSCRT_ROOT}/presets/*.json)
set(stamp ${CMAKE_BINARY_DIR}/ntscrt_resources.stamp)
add_custom_command(OUTPUT ${stamp}
  COMMAND ${CMAKE_COMMAND} -DMANIFEST=${manifest} -DSRC=${NTSCRT_SLANG_SHADERS_DIR} -DDST=${res}/shaders
          -DPRESETS=${NTSCRT_ROOT}/presets -DPRESETS_DST=${res}/presets
          -P ${CMAKE_CURRENT_SOURCE_DIR}/copy_resources.cmake
  COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
  DEPENDS ${manifest} ${shader_srcs} ${preset_files} ${CMAKE_CURRENT_SOURCE_DIR}/copy_resources.cmake
  COMMENT "Assembling ${res} (CRT shader subset, presets)"
  VERBATIM)

add_custom_target(ntscrt_resources ALL DEPENDS ${lib_out} ${stamp})
add_dependencies(ntscrt_core ntscrt_resources)
install(DIRECTORY ${res}/ DESTINATION resources)
