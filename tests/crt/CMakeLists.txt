ntscrt_add_test(test_crt test_crt.cpp)
target_compile_definitions(test_crt PRIVATE NTSCRT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}")
