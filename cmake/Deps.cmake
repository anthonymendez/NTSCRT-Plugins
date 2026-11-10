# Third-party dependencies fetched at configure time (pinned).
include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # doctest 2.4.11 declares cmake 3.0; CMake 4 needs this

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11 GIT_SHALLOW TRUE)
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 5c205738c191bcb0abc65c4febfa9bd25ff35234)  # 2024-05 master
FetchContent_MakeAvailable(nlohmann_json doctest stb)

add_library(ntscrt_stb INTERFACE)
target_include_directories(ntscrt_stb INTERFACE ${stb_SOURCE_DIR})

# ntsc-rs C ABI (agent A): Rust staticlib built by cargo -> imported target `ntscrs_capi`.
add_subdirectory(${NTSCRT_ROOT}/rust/ntscrs-capi ${CMAKE_BINARY_DIR}/ntscrs-capi)
