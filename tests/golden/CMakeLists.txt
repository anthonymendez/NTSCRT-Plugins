# Golden comparison against upstream NTSCRT crt-smoke references (owner: agent F).
# References: tests/golden/ref/<id>.png (+ .json sidecar) from .github/workflows/golden.yml.
# Entries without a reference are skipped, so the test passes until refs are committed.
set(NTSCRT_GOLDEN_RESOURCE_DIR "${NTSCRT_ROOT}" CACHE PATH
    "resource_dir passed to ntscrt_create by test_golden (needs presets/ and shaders/); env NTSCRT_RESOURCE_DIR overrides")

ntscrt_add_test(test_golden test_golden.cpp)
target_compile_definitions(test_golden PRIVATE
  NTSCRT_GOLDEN_RESOURCE_DIR="${NTSCRT_GOLDEN_RESOURCE_DIR}"
  NTSCRT_GOLDEN_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden-out")
set_tests_properties(test_golden PROPERTIES LABELS golden)

add_executable(ntscrt-imgdiff imgdiff.cpp)
target_link_libraries(ntscrt-imgdiff PRIVATE nlohmann_json::nlohmann_json ntscrt_stb)
