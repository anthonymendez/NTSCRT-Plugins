# Owner: agent G. Screen Loop (howlaround): port of upstream Howlaround.swift.
set(_howl ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(ntscrt_core PRIVATE
  ${_howl}/howlaround.cpp
  ${_howl}/howl_math.cpp
  ${_howl}/howl_counter.cpp
  ${_howl}/howl_cpu.cpp)
# stb_truetype's rasterizer for the camcorder counter (STBTT_STATIC: private copy).
target_link_libraries(ntscrt_core PRIVATE ntscrt_stb)
ntscrt_add_shaders(ntscrt_core
  ${_howl}/shaders/howl_camera.slang
  ${_howl}/shaders/howl_blur.slang
  ${_howl}/shaders/howl_mean.slang
  ${_howl}/shaders/howl_resample.slang)
