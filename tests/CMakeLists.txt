# Unit tests (doctest). Each owner adds test files for its area to
# tests/<area>/ and registers them with ntscrt_add_test() in tests/<area>/CMakeLists.txt.
function(ntscrt_add_test name)
  add_executable(${name} ${ARGN} ${NTSCRT_ROOT}/tests/doctest_main.cpp)
  target_link_libraries(${name} PRIVATE ntscrt_core doctest::doctest ntscrt_stb)
  target_include_directories(${name} PRIVATE ${NTSCRT_ROOT}/core ${NTSCRT_ROOT}/core/src)
  target_compile_definitions(${name} PRIVATE NTSCRT_SOURCE_DIR="${NTSCRT_ROOT}" NTSCRT_BUILT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}")
  if(TARGET ntscrt_resources)
    add_dependencies(${name} ntscrt_resources)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

ntscrt_add_test(test_abi abi/test_abi.cpp)

file(GLOB area_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(d ${area_dirs})
  if(IS_DIRECTORY ${d} AND EXISTS ${d}/CMakeLists.txt)
    add_subdirectory(${d})
  endif()
endforeach()
