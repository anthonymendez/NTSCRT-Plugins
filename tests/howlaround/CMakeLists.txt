ntscrt_add_test(test_howlaround
  ${CMAKE_CURRENT_SOURCE_DIR}/test_howl_math.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_howl_loop.cpp)

# Screen Loop playback / seek timings, run by hand (not a ctest test).
add_executable(bench_screenloop bench_screenloop.cpp)
target_link_libraries(bench_screenloop PRIVATE ntscrt_core)
target_compile_definitions(bench_screenloop PRIVATE NTSCRT_SOURCE_DIR="${NTSCRT_ROOT}"
  NTSCRT_BUILT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}")
if(TARGET ntscrt_resources)
  add_dependencies(bench_screenloop ntscrt_resources)
endif()

# Screen Loop through the C ABI on the GPU: seeks = playing in order.
ntscrt_add_test(test_howl_seek ${CMAKE_CURRENT_SOURCE_DIR}/test_howl_seek.cpp)
