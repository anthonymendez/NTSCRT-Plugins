# One-off: the OpenFX plug-in's looks per bundled preset, from the param model
# before the shared host library (writes ofx_reference.json).
add_executable(gen_ofx_reference gen_ofx_reference.cpp ${NTSCRT_ROOT}/ofx/ofx_params.cpp)
target_include_directories(gen_ofx_reference PRIVATE ${NTSCRT_ROOT}/ofx ${NTSCRT_ROOT}/core)
target_link_libraries(gen_ofx_reference PRIVATE ntscrt_core nlohmann_json::nlohmann_json)
target_compile_definitions(gen_ofx_reference PRIVATE NTSCRT_BUILT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}")
if(TARGET ntscrt_resources)
  add_dependencies(gen_ofx_reference ntscrt_resources)
endif()
