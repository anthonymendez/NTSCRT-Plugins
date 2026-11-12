# ntscrt_core — static library linked into the OFX bundle, the FxPlug XPC
# service and ntscrt-cli. Stage owners add sources ONLY in their own
# core/stages/<name>/CMakeLists.txt (and core/gpu/CMakeLists.txt for agent D).
add_library(ntscrt_core STATIC
  src/ntscrt.cpp
  src/pixel_convert.cpp
  src/frames_needed.cpp
  src/host_gpu.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/embedded_schema.cpp)

ntscrt_embed_text(${CMAKE_CURRENT_BINARY_DIR}/embedded_schema.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/params/schema.json ntscrt_embedded_schema)

target_include_directories(ntscrt_core
  PUBLIC include
  PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ntscrt_core PRIVATE NTSCRT_BUILDING)
target_link_libraries(ntscrt_core PUBLIC nlohmann_json::nlohmann_json)

add_subdirectory(gpu)
ntscrt_add_shaders(ntscrt_core ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/facade.slang)
# Host GPU frames (ntscrt_render_host_gpu): CUDA <-> D3D11 on Windows, MTLBuffer <-> Metal on macOS.
ntscrt_add_shaders(ntscrt_core ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/hostgpu_in.slang
  ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/hostgpu_flags.slang ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/hostgpu_out.slang)
if(WIN32)
  target_sources(ntscrt_core PRIVATE src/host_gpu_cuda.cpp)
elseif(APPLE)
  target_sources(ntscrt_core PRIVATE src/host_gpu_metal.mm)
  set_source_files_properties(src/host_gpu_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
foreach(stage crop digital ntsc downscale glitch crt howlaround)
  add_subdirectory(stages/${stage})
endforeach()
