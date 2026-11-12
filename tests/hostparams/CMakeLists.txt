# The shared host param model (hostparams/) both plug-ins use: its rules, the
# OpenFX plug-in's looks before / after the merge, and the two hosts agreeing.
ntscrt_add_test(test_hostparams
  test_params_model.cpp
  test_params_ofx.cpp
  test_params_timeline.cpp
  test_ofx_reference.cpp
  test_debug_switch.cpp)
target_link_libraries(test_hostparams PRIVATE ntscrt_hostparams)

ntscrt_add_test(test_hostparams_cross test_cross_host.cpp)
target_link_libraries(test_hostparams_cross PRIVATE ntscrt_fx_model)
