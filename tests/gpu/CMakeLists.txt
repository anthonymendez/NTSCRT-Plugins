ntscrt_add_test(test_gpu test_gpu.cpp)
ntscrt_add_shaders(test_gpu ${CMAKE_CURRENT_SOURCE_DIR}/roundtrip.slang)
if(WIN32 AND CMAKE_HOST_WIN32 AND NTSCRT_PRECOMPILE_DXBC)
  target_compile_definitions(test_gpu PRIVATE NTSCRT_EXPECT_DXBC)  # every kernel embeds its bytecode
endif()
