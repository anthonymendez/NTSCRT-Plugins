# Host GPU frames: ntscrt_render_host_gpu vs ntscrt_render_cpu (fake CUDA host on Windows).
ntscrt_add_test(test_host_gpu test_host_gpu.cpp)
target_link_libraries(test_host_gpu PRIVATE ntscrt_hostparams)  # the plug-in's default look
# Clip mode's hole filling on the GPU: byte parity with fill_transparent (GPU and WARP, no CUDA).
ntscrt_add_test(test_fill_gpu test_fill_gpu.cpp)
