ntscrt_add_test(test_gpu test_gpu.cpp)
ntscrt_add_shaders(test_gpu ${CMAKE_CURRENT_SOURCE_DIR}/roundtrip.slang)
