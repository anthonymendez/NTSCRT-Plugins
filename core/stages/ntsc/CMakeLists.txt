# Owner: agent A. Add this stage's sources/shaders/tests here only.
target_sources(ntscrt_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/ntsc.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ntsc_internal.h)
# ntsc-rs C ABI (rust/ntscrs-capi, imported target defined from cmake/Deps.cmake).
target_link_libraries(ntscrt_core PUBLIC ntscrs_capi)
add_dependencies(ntscrt_core ntscrs_capi_build)
# GPU port of the effect (docs/dev/NTSC-GPU.md); the Rust path above stays the reference and fallback.
target_sources(ntscrt_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/gpu/ntsc_gpu.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gpu/ntsc_gpu.h)
ntscrt_add_shaders(ntscrt_core ${CMAKE_CURRENT_SOURCE_DIR}/gpu/shaders/ntsc_gpu.slang)
