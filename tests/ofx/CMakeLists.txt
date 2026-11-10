# Mock-OpenFX-host test of the built NTSCRT.ofx bundle (agent E).
if(TARGET ntscrt_ofx)
  ntscrt_add_test(test_ofx test_ofx.cpp mock_host.cpp)
  target_link_libraries(test_ofx PRIVATE ntscrt_ofx_headers)
  target_compile_definitions(test_ofx PRIVATE NTSCRT_OFX_BINARY="$<TARGET_FILE:ntscrt_ofx>")
  add_dependencies(test_ofx ntscrt_ofx)
  if(UNIX)
    target_link_libraries(test_ofx PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

# The host-independent param model (ofx/ofx_params.*), compiled straight in.
ntscrt_add_test(test_ofx_params test_ofx_params.cpp ${NTSCRT_ROOT}/ofx/ofx_params.cpp)
target_include_directories(test_ofx_params PRIVATE ${NTSCRT_ROOT}/ofx)
target_link_libraries(test_ofx_params PRIVATE nlohmann_json::nlohmann_json)
