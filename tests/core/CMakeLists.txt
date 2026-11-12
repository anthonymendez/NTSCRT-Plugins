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

# Transfer / conversion benchmark, run by hand (not a ctest test): per-frame and
# per-phase times of ntscrt_render_cpu as an OpenFX host calls it.
add_executable(bench_transfer bench_transfer.cpp)
target_link_libraries(bench_transfer PRIVATE ntscrt_core ntscrt_hostparams nlohmann_json::nlohmann_json)
target_include_directories(bench_transfer PRIVATE ${NTSCRT_ROOT}/core ${NTSCRT_ROOT}/core/src)
target_compile_definitions(bench_transfer PRIVATE NTSCRT_BUILT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}")
if(TARGET ntscrt_resources)
  add_dependencies(bench_transfer ntscrt_resources)
endif()

ntscrt_add_test(test_core_device test_shared_device.cpp)
ntscrt_add_test(test_core_bitdepth test_bit_depth.cpp)
ntscrt_add_test(test_core_convert test_convert_threads.cpp)
