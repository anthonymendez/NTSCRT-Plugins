ntscrt_add_test(test_digital test_digital.cpp)
# The GPU route (docs/dev/DIGITAL-GPU.md): byte parity with the CPU code, kernels and pipeline.
ntscrt_add_test_exe(test_digital_gpu test_digital_gpu.cpp)
target_link_libraries(test_digital_gpu PRIVATE ntscrt_hostparams)  # the plug-in's default look, formats
ntscrt_split_test(test_digital_gpu COMMAND test_digital_gpu PARTS  # for ctest -j (tests/CMakeLists.txt)
  formats "digital gpu pipeline: every Digital video format*")
set_tests_properties(test_digital_gpu.formats test_digital_gpu.rest PROPERTIES COST 70)

# Digital video seek timings, run by hand (not a ctest test; docs/dev/DIGITAL-CHECKPOINTS.md).
add_executable(bench_digital bench_digital.cpp)
target_link_libraries(bench_digital PRIVATE ntscrt_core ntscrt_hostparams)
if(WIN32)
  target_link_libraries(bench_digital PRIVATE psapi)
endif()
target_compile_definitions(bench_digital PRIVATE NTSCRT_BUILT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}")
if(TARGET ntscrt_resources)
  add_dependencies(bench_digital ntscrt_resources)
endif()

# Seeks = playing in order through the C ABI, every format; checkpoints bound the re-runs.
ntscrt_add_test(test_digital_seek test_digital_seek.cpp)
target_link_libraries(test_digital_seek PRIVATE ntscrt_hostparams)
