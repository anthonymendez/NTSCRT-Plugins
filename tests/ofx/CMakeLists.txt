# Mock-OpenFX-host test of the built NTSCRT.ofx bundle (agent E).
if(TARGET ntscrt_ofx)
  ntscrt_add_test(test_ofx test_ofx.cpp mock_host.cpp)
  target_link_libraries(test_ofx PRIVATE ntscrt_ofx_headers ntscrt_hostparams)
  target_compile_definitions(test_ofx PRIVATE NTSCRT_OFX_BINARY="$<TARGET_FILE:ntscrt_ofx>")
  add_dependencies(test_ofx ntscrt_ofx)
  # The host protocol as DaVinci Resolve sees it (getFramesNeeded leaves out Screen Loop's history).
  add_test(NAME test_ofx_resolve_host COMMAND test_ofx "--test-case=host protocol: getFramesNeeded*")
  set_tests_properties(test_ofx_resolve_host PROPERTIES ENVIRONMENT "NTSCRT_MOCK_HOST_NAME=DaVinciResolve")
  # Screen Loop's store of its window's source frames on (test_ofx turns it off otherwise)
  add_test(NAME test_ofx_screenloop_store COMMAND test_ofx "--test-case=Screen Loop: *")
  set_tests_properties(test_ofx_screenloop_store PROPERTIES ENVIRONMENT "NTSCRT_SCREENLOOP_SOURCE_MB=2048")
  if(UNIX)
    target_link_libraries(test_ofx PRIVATE ${CMAKE_DL_LIBS})
  endif()

  # The installers' shader warm-up: the bundle's ntscrt-warm into an empty folder, then
  # the plug-in in another process renders every CRT preset compiling nothing.
  set(warm_dir ${CMAKE_CURRENT_BINARY_DIR}/warm-tool-cache)
  add_test(NAME warm_tool_clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${warm_dir} ${warm_dir}-timeout)
  set_tests_properties(warm_tool_clean PROPERTIES FIXTURES_SETUP warm_tool_clean)
  add_test(NAME warm_tool COMMAND $<TARGET_FILE:ntscrt_warm> --timeout 600)
  set_tests_properties(warm_tool PROPERTIES ENVIRONMENT "NTSCRT_SHADER_CACHE=${warm_dir}"
                       FIXTURES_REQUIRED warm_tool_clean FIXTURES_SETUP warm_tool_cache)  # exit 0: every preset built
  add_test(NAME test_ofx_after_warmup COMMAND test_ofx "--test-case=shader warm-up: after the installer*")
  set_tests_properties(test_ofx_after_warmup PROPERTIES FIXTURES_REQUIRED warm_tool_cache ENVIRONMENT
    "NTSCRT_SHADER_CACHE=${warm_dir};NTSCRT_TEST_AFTER_WARMUP=1;NTSCRT_SHADER_WARMUP=1;NTSCRT_SHADER_WARMUP_DELAY_MS=0")
  # Its time limit, and a plug-in it can't load: one line each, never a hang.
  add_test(NAME warm_tool_timeout COMMAND $<TARGET_FILE:ntscrt_warm> --timeout 0.001)
  set_tests_properties(warm_tool_timeout PROPERTIES ENVIRONMENT "NTSCRT_SHADER_CACHE=${warm_dir}-timeout"
                       FIXTURES_REQUIRED warm_tool_clean PASS_REGULAR_EXPRESSION "stopped after" TIMEOUT 60)
  add_test(NAME warm_tool_no_plugin COMMAND $<TARGET_FILE:ntscrt_warm> --plugin ${CMAKE_CURRENT_BINARY_DIR}/missing.ofx)
  set_tests_properties(warm_tool_no_plugin PROPERTIES PASS_REGULAR_EXPRESSION "couldn't load the plug-in" TIMEOUT 60)
endif()

# The param model the plug-in uses is the shared host library: tests/hostparams.
