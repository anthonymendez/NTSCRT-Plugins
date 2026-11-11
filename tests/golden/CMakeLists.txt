# Golden tests (docs/TESTING.md "Golden images"):
#  - test_golden.cpp: ntscrt_core vs upstream NTSCRT crt-smoke references, tests/golden/ref/<id>.png
#    (+ .json sidecar) from .github/workflows/golden.yml.
#  - test_golden_self.cpp: ntscrt_core vs its own baselines, tests/golden/self/ (manifest.json).
# Missing references are skipped unless NTSCRT_GOLDEN_STRICT=1 (CI).
set(NTSCRT_GOLDEN_RESOURCE_DIR "${NTSCRT_RESOURCES_BUILD_DIR}" CACHE PATH
    "resource_dir passed to ntscrt_create by test_golden (needs presets/ and shaders/); env NTSCRT_RESOURCE_DIR overrides")

ntscrt_add_test(test_golden test_golden.cpp test_golden_self.cpp)
target_compile_definitions(test_golden PRIVATE
  NTSCRT_GOLDEN_RESOURCE_DIR="${NTSCRT_GOLDEN_RESOURCE_DIR}"
  NTSCRT_GOLDEN_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden-out")
set_tests_properties(test_golden PROPERTIES LABELS golden)

add_executable(ntscrt-imgdiff imgdiff.cpp)
target_link_libraries(ntscrt-imgdiff PRIVATE nlohmann_json::nlohmann_json ntscrt_stb)

# Regenerate the self-references after an intended look change:
#   cmake --build <build> --config Release --target golden-update
# then review `git diff --stat tests/golden/self` and the images, and commit.
add_custom_target(golden-update
  COMMAND ${CMAKE_COMMAND} -E env NTSCRT_GOLDEN_UPDATE=1 $<TARGET_FILE:test_golden> "--test-case=golden self*"
  DEPENDS test_golden
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  COMMENT "Rendering tests/golden/self/ref baselines"
  VERBATIM)
