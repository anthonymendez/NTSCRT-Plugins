# Owner: agent D. Add this stage's sources/shaders/tests here only.
target_sources(ntscrt_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/downscale.cpp)
ntscrt_add_shaders(ntscrt_core ${CMAKE_CURRENT_SOURCE_DIR}/shaders/downscale.slang)
