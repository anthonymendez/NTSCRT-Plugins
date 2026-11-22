# Unit tests (doctest). Each owner adds test files for its area to
# tests/<area>/ and registers them with ntscrt_add_test() in tests/<area>/CMakeLists.txt.
# ntscrt_add_test_exe(): the executable only, for ntscrt_split_test below.
function(ntscrt_add_test_exe name)
  add_executable(${name} ${ARGN} ${NTSCRT_ROOT}/tests/doctest_main.cpp)
  target_link_libraries(${name} PRIVATE ntscrt_core doctest::doctest ntscrt_stb)
  target_include_directories(${name} PRIVATE ${NTSCRT_ROOT}/core ${NTSCRT_ROOT}/core/src)
  target_compile_definitions(${name} PRIVATE NTSCRT_SOURCE_DIR="${NTSCRT_ROOT}" NTSCRT_BUILT_RESOURCES_DIR="${NTSCRT_RESOURCES_BUILD_DIR}"
    NTSCRT_TEST_SHADER_CACHE_DIR="${CMAKE_BINARY_DIR}/test-shader-cache")  # doctest_main.cpp
  if(TARGET ntscrt_resources)
    add_dependencies(${name} ntscrt_resources)
  endif()
endfunction()
function(ntscrt_add_test name)
  ntscrt_add_test_exe(${name} ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# ntscrt_split_test(<name> COMMAND <target> [BASE <filter>] [ENVIRONMENT <var=value>...]
#                   PARTS <label> <filter> [<label> <filter>...])
# Runs a long doctest executable as several ctest entries, so `ctest -j` spreads its cases
# over the CPUs (docs/TESTING.md "Running tests in parallel"):
#   <name>.<label>      the cases matching <filter> (a doctest --test-case= list: wildcard
#                       patterns separated by commas; "\," for a comma in a name)
#   <name>.rest         all the others
#   <name>.split_check  fails unless each case runs in exactly one of them (tests/split_check.cmake)
# BASE: only the cases matching it (a variant running a subset in another environment); each
# part's filter must match only cases inside it.
# ENVIRONMENT applies to every entry, the check included (it lists with the same skips).
function(ntscrt_split_test name)
  cmake_parse_arguments(PARSE_ARGV 1 ST "" "COMMAND;BASE" "ENVIRONMENT;PARTS")
  list(LENGTH ST_PARTS n)
  math(EXPR odd "${n} % 2")
  if(n EQUAL 0 OR odd)
    message(FATAL_ERROR "ntscrt_split_test(${name}): PARTS needs <label> <filter> pairs")
  endif()
  set(base_args "")
  if(ST_BASE)
    set(base_args "--test-case=${ST_BASE}")
  endif()
  set(tests ${name}.rest ${name}.split_check)
  set(filters "")
  math(EXPR last "${n} - 1")
  foreach(i RANGE 0 ${last} 2)
    math(EXPR j "${i} + 1")
    list(GET ST_PARTS ${i} label)
    list(GET ST_PARTS ${j} filter)
    if(filter MATCHES "[|;]")
      message(FATAL_ERROR "ntscrt_split_test(${name}): no | or ; in filters (${filter})")
    endif()
    # Not with BASE: two --test-case filters match either (the check fails a part outside BASE).
    add_test(NAME ${name}.${label} COMMAND ${ST_COMMAND} "--test-case=${filter}")
    list(APPEND tests ${name}.${label})
    list(APPEND filters "${filter}")
  endforeach()
  list(JOIN filters "," excludes)
  add_test(NAME ${name}.rest COMMAND ${ST_COMMAND} ${base_args} "--test-case-exclude=${excludes}")
  list(JOIN filters "|" parts)
  add_test(NAME ${name}.split_check COMMAND ${CMAKE_COMMAND} "-DEXE=$<TARGET_FILE:${ST_COMMAND}>"
           "-DBASE=${ST_BASE}" "-DPARTS=${parts}" -P ${NTSCRT_ROOT}/tests/split_check.cmake)
  if(ST_ENVIRONMENT)
    set_tests_properties(${tests} PROPERTIES ENVIRONMENT "${ST_ENVIRONMENT}")
  endif()
endfunction()

ntscrt_add_test(test_abi abi/test_abi.cpp)

file(GLOB area_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(d ${area_dirs})
  if(IS_DIRECTORY ${d} AND EXISTS ${d}/CMakeLists.txt)
    add_subdirectory(${d})
  endif()
endforeach()
