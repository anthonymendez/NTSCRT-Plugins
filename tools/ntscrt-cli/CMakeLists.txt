add_executable(ntscrt-cli main.cpp)
target_link_libraries(ntscrt-cli PRIVATE ntscrt_core ntscrt_stb nlohmann_json::nlohmann_json)
