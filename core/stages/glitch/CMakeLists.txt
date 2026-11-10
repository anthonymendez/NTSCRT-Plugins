# Owner: agent B. Glitch stage: TV/VCR receiver simulation (CPU) + GPU compose.
target_sources(ntscrt_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/receiver.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/glitch_kernels.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/glitch_timeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/glitch.cpp)
ntscrt_add_shaders(ntscrt_core ${CMAKE_CURRENT_SOURCE_DIR}/shaders/glitch.slang)
