ntscrt_add_test_exe(test_howlaround
  ${CMAKE_CURRENT_SOURCE_DIR}/test_howl_math.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_howl_loop.cpp)
ntscrt_split_test(test_howlaround COMMAND test_howlaround PARTS
  checkpoints "checkpoints and pipelined passes*")
set_tests_properties(test_howlaround.checkpoints test_howlaround.rest PROPERTIES COST 120)

# Screen Loop playback / seek timings, run by hand (not a ctest test).
add_executable(bench_screenloop bench_screenloop.cpp)
target_link_libraries(bench_screenloop PRIVATE ntscrt_core)
if(WIN32)
  target_link_libraries(bench_screenloop PRIVATE psapi)
endif()
target_compile_definitions(bench_screenloop PRIVATE NTSCRT_SOURCE_DIR="${NTSCRT_ROOT}"
  NTSCRT_BUILT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}")
if(TARGET ntscrt_resources)
  add_dependencies(bench_screenloop ntscrt_resources)
endif()

# Screen Loop through the C ABI on the GPU: seeks = playing in order.
# In parts for `ctest -j` (tests/CMakeLists.txt: ntscrt_split_test); COST: seconds on GitHub's
# Windows runner (WARP), the long ones start first.
ntscrt_add_test_exe(test_howl_seek ${CMAKE_CURRENT_SOURCE_DIR}/test_howl_seek.cpp)
ntscrt_split_test(test_howl_seek COMMAND test_howl_seek PARTS
  scrub "Screen Loop: seeks and scrubbing*"
  ntsc "Screen Loop: NTSC on the GPU*"
  settings "Screen Loop: other settings*"
  with "Screen Loop with *")
set_tests_properties(test_howl_seek.scrub test_howl_seek.ntsc PROPERTIES COST 300)
set_tests_properties(test_howl_seek.settings test_howl_seek.with test_howl_seek.rest PROPERTIES COST 200)
