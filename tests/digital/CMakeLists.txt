ntscrt_add_test(test_digital test_digital.cpp)
# The GPU route (docs/dev/DIGITAL-GPU.md): byte parity with the CPU code, kernels and pipeline.
ntscrt_add_test(test_digital_gpu test_digital_gpu.cpp)
target_link_libraries(test_digital_gpu PRIVATE ntscrt_hostparams)  # the plug-in's default look, formats
