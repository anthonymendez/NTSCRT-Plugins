ntscrt_add_test(test_crt test_crt.cpp)
target_compile_definitions(test_crt PRIVATE NTSCRT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}")

# Render order: shared chains (writes the hashes), then a second process with
# unshared chains (checks against them).
ntscrt_add_test(test_crt_order test_crt_order.cpp)
target_link_libraries(test_crt_order PRIVATE ntscrt_hostparams)  # the plug-in's default look
set(crt_order_hashes ${CMAKE_CURRENT_BINARY_DIR}/crt_order_hashes.txt)
set_tests_properties(test_crt_order PROPERTIES ENVIRONMENT "NTSCRT_ORDER_HASHES=${crt_order_hashes}"
                     FIXTURES_SETUP crt_order_hashes)
add_test(NAME test_crt_order_unshared COMMAND test_crt_order)
set_tests_properties(test_crt_order_unshared PROPERTIES
                     ENVIRONMENT "NTSCRT_ORDER_HASHES=${crt_order_hashes};NTSCRT_SHARED_CHAINS=0"
                     FIXTURES_REQUIRED crt_order_hashes)

# librashader's shader cache: its own temporary folder (the test sets it), then a second
# process with the cache off. NTSCRT_SHARED_CHAINS=0: each stage builds its own chain.
ntscrt_add_test(test_shader_cache test_shader_cache.cpp)
target_compile_definitions(test_shader_cache PRIVATE NTSCRT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}")
set_tests_properties(test_shader_cache PROPERTIES ENVIRONMENT "NTSCRT_SHARED_CHAINS=0")
add_test(NAME test_shader_cache_off COMMAND test_shader_cache)
set_tests_properties(test_shader_cache_off PROPERTIES
                     ENVIRONMENT "NTSCRT_SHADER_CACHE=0;NTSCRT_TEST_SHADER_CACHE_OFF=1;NTSCRT_SHARED_CHAINS=0")
