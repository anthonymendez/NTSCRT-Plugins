# The shared host param model (hostparams/) both plug-ins use: its rules, the
# OpenFX plug-in's looks before / after the merge, and the two hosts agreeing.
ntscrt_add_test_exe(test_hostparams
  test_params_model.cpp
  test_params_ofx.cpp
  test_params_timeline.cpp
  test_ofx_reference.cpp
  test_debug_switch.cpp)
target_link_libraries(test_hostparams PRIVATE ntscrt_hostparams)
# In parts for ctest -j (tests/CMakeLists.txt). The debug switch's cases wait for its poll
# interval (50 ms): alone (RUN_SERIAL) and first, so a busy machine can't fail them.
ntscrt_split_test(test_hostparams COMMAND test_hostparams PARTS
  sparse "animated preset: sparse keys*"
  debug_switch "debug switch: *")
set_tests_properties(test_hostparams.debug_switch PROPERTIES RUN_SERIAL TRUE COST 100000)
set_tests_properties(test_hostparams.sparse PROPERTIES COST 110)

ntscrt_add_test(test_hostparams_cross test_cross_host.cpp)
target_link_libraries(test_hostparams_cross PRIVATE ntscrt_fx_model)
