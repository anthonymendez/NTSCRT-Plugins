ntscrt_add_test(test_ntsc test_ntsc.cpp)
# GPU port of the stage: parity with ntsc-rs, fallback rules (docs/dev/NTSC-GPU.md).
ntscrt_add_test_exe(test_ntsc_gpu test_ntsc_gpu.cpp)
ntscrt_split_test(test_ntsc_gpu COMMAND test_ntsc_gpu PARTS  # for ctest -j (tests/CMakeLists.txt)
  presets "ntsc gpu: parity on every bundled preset*"
  passes "ntsc gpu: parity with ntsc-rs?*")
set_tests_properties(test_ntsc_gpu.presets test_ntsc_gpu.passes PROPERTIES COST 120)

# Dumps ntsc-rs descriptors + house defaults for tools/gen_schema.py
# (snapshot committed as core/params/ntsc_descriptors.json).
add_executable(ntsc-dump dump_ntsc.cpp)
target_link_libraries(ntsc-dump PRIVATE ntscrt_core)
target_include_directories(ntsc-dump PRIVATE ${NTSCRT_ROOT}/core ${NTSCRT_ROOT}/core/src)
