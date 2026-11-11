# Core tests owned by the bit-depth / shared-device work (agent G).
if(WIN32)
  # Benchmark, run by hand (not a ctest test).
  add_executable(bench_instances bench_instances.cpp)
  target_link_libraries(bench_instances PRIVATE ntscrt_core)
  target_include_directories(bench_instances PRIVATE ${NTSCRT_ROOT}/core)
  target_compile_definitions(bench_instances PRIVATE NTSCRT_BUILT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}")
  if(TARGET ntscrt_resources)
    add_dependencies(bench_instances ntscrt_resources)
  endif()
endif()

ntscrt_add_test(test_core_device test_shared_device.cpp)
ntscrt_add_test(test_core_bitdepth test_bit_depth.cpp)
ntscrt_add_test(test_core_convert test_convert_threads.cpp)
