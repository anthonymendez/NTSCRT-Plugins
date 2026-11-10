# ntscrt_core — static library linked into the OFX bundle, the FxPlug XPC
# service and ntscrt-cli. Stage owners add sources ONLY in their own
# core/stages/<name>/CMakeLists.txt (and core/gpu/CMakeLists.txt for agent D).
add_library(ntscrt_core STATIC
  src/ntscrt.cpp
  src/pixel_convert.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/embedded_schema.cpp)

ntscrt_embed_text(${CMAKE_CURRENT_BINARY_DIR}/embedded_schema.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/params/schema.json ntscrt_embedded_schema)

target_include_directories(ntscrt_core
  PUBLIC include
  PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ntscrt_core PRIVATE NTSCRT_BUILDING)
target_link_libraries(ntscrt_core PUBLIC nlohmann_json::nlohmann_json)

add_subdirectory(gpu)
foreach(stage crop digital ntsc downscale glitch crt howlaround)
  add_subdirectory(stages/${stage})
endforeach()
