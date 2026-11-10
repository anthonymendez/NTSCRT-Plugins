# Builds rust/ntscrs-capi (C ABI over vendor/ntsc-rs) as a Rust staticlib and
# exposes it as the imported target `ntscrs_capi`.
#   Windows: x86_64-pc-windows-msvc, static CRT (+crt-static, .cargo/config.toml)
#   macOS:   aarch64-apple-darwin + x86_64-apple-darwin, lipo'd universal .a
# Cargo always builds its release profile (the effect is far too slow in debug).

set(NTSCRS_CAPI_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(NTSCRS_VENDOR_DIR ${NTSCRT_ROOT}/vendor/ntsc-rs)
if(NOT EXISTS ${NTSCRS_VENDOR_DIR}/crates/ntscrs/Cargo.toml)
  message(FATAL_ERROR "vendor/ntsc-rs is missing: run `git submodule update --init vendor/ntsc-rs`")
endif()

find_program(NTSCRT_CARGO cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin")
if(NOT NTSCRT_CARGO)
  message(FATAL_ERROR "cargo not found (install Rust stable: https://rustup.rs)")
endif()

set(NTSCRS_CARGO_TARGET_DIR ${CMAKE_BINARY_DIR}/cargo-ntscrs)
file(GLOB_RECURSE NTSCRS_RUST_SOURCES CONFIGURE_DEPENDS
  ${NTSCRS_CAPI_DIR}/src/*.rs
  ${NTSCRS_VENDOR_DIR}/crates/ntscrs/src/*.rs
  ${NTSCRS_VENDOR_DIR}/crates/macros/src/*.rs)
set(NTSCRS_DEPENDS
  ${NTSCRS_RUST_SOURCES}
  ${NTSCRS_CAPI_DIR}/Cargo.toml
  ${NTSCRS_CAPI_DIR}/Cargo.lock
  ${NTSCRS_CAPI_DIR}/.cargo/config.toml
  ${NTSCRS_VENDOR_DIR}/crates/ntscrs/Cargo.toml
  ${NTSCRS_VENDOR_DIR}/crates/macros/Cargo.toml)

set(_cargo_build ${NTSCRT_CARGO} build --release --locked
  --manifest-path ${NTSCRS_CAPI_DIR}/Cargo.toml)

if(APPLE)
  set(_arm ${NTSCRS_CARGO_TARGET_DIR}/aarch64-apple-darwin/release/libntscrs_capi.a)
  set(_x86 ${NTSCRS_CARGO_TARGET_DIR}/x86_64-apple-darwin/release/libntscrs_capi.a)
  set(NTSCRS_CAPI_LIB ${NTSCRS_CARGO_TARGET_DIR}/universal/libntscrs_capi.a)
  set(_env ${CMAKE_COMMAND} -E env
    CARGO_TARGET_DIR=${NTSCRS_CARGO_TARGET_DIR}
    MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
  add_custom_command(
    OUTPUT ${NTSCRS_CAPI_LIB}
    COMMAND ${_env} ${_cargo_build} --target aarch64-apple-darwin
    COMMAND ${_env} ${_cargo_build} --target x86_64-apple-darwin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${NTSCRS_CARGO_TARGET_DIR}/universal
    COMMAND lipo -create ${_arm} ${_x86} -output ${NTSCRS_CAPI_LIB}
    DEPENDS ${NTSCRS_DEPENDS}
    WORKING_DIRECTORY ${NTSCRS_CAPI_DIR}
    BYPRODUCTS ${_arm} ${_x86}
    COMMENT "cargo: ntscrs-capi (universal arm64+x86_64)"
    USES_TERMINAL VERBATIM)
elseif(WIN32)
  set(_triple x86_64-pc-windows-msvc)
  set(NTSCRS_CAPI_LIB ${NTSCRS_CARGO_TARGET_DIR}/${_triple}/release/ntscrs_capi.lib)
  add_custom_command(
    OUTPUT ${NTSCRS_CAPI_LIB}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${NTSCRS_CARGO_TARGET_DIR}
            ${_cargo_build} --target ${_triple}
    DEPENDS ${NTSCRS_DEPENDS}
    WORKING_DIRECTORY ${NTSCRS_CAPI_DIR}   # picks up .cargo/config.toml (+crt-static)
    COMMENT "cargo: ntscrs-capi (${_triple}, static CRT)"
    USES_TERMINAL VERBATIM)
else()
  set(NTSCRS_CAPI_LIB ${NTSCRS_CARGO_TARGET_DIR}/release/libntscrs_capi.a)
  add_custom_command(
    OUTPUT ${NTSCRS_CAPI_LIB}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${NTSCRS_CARGO_TARGET_DIR} ${_cargo_build}
    DEPENDS ${NTSCRS_DEPENDS}
    WORKING_DIRECTORY ${NTSCRS_CAPI_DIR}
    COMMENT "cargo: ntscrs-capi"
    USES_TERMINAL VERBATIM)
endif()

add_custom_target(ntscrs_capi_build DEPENDS ${NTSCRS_CAPI_LIB})

add_library(ntscrs_capi STATIC IMPORTED GLOBAL)
set_target_properties(ntscrs_capi PROPERTIES
  IMPORTED_LOCATION ${NTSCRS_CAPI_LIB}
  INTERFACE_INCLUDE_DIRECTORIES ${NTSCRS_CAPI_DIR}/include)
add_dependencies(ntscrs_capi ntscrs_capi_build)

if(WIN32)
  # Rust std's system deps (`cargo rustc -- --print native-static-libs`).
  set_property(TARGET ntscrs_capi PROPERTY INTERFACE_LINK_LIBRARIES
    kernel32 advapi32 ntdll userenv ws2_32 bcrypt dbghelp
    # Rust is built against the release static CRT (libcmt); Debug C++ uses libcmtd.
    "$<$<CONFIG:Debug>:-NODEFAULTLIB:libcmt.lib>")
elseif(APPLE)
  set_property(TARGET ntscrs_capi PROPERTY INTERFACE_LINK_LIBRARIES "-lc" "-lm")
else()
  set_property(TARGET ntscrs_capi PROPERTY INTERFACE_LINK_LIBRARIES pthread dl m)
endif()
