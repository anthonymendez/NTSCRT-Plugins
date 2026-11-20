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
endif()

# The param model the plug-in uses is the shared host library: tests/hostparams.
