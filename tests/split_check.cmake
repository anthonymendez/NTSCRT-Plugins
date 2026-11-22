# cmake -DEXE=<doctest exe> [-DBASE=<filter>] -DPARTS=<filter>|<filter>|... -P split_check.cmake
#
# The check behind ntscrt_split_test (tests/CMakeLists.txt): the ctest entries one doctest
# executable was split into run each of its test cases exactly once. Lists the cases the whole
# executable runs (those matching BASE, when given), each part's (--test-case=<filter>) and the
# rest's (every part filter excluded), and fails when a case is in no list or in two, or when a
# part matches no case at all (a filter left behind by a renamed case).
if(NOT EXE OR NOT DEFINED PARTS)
  message(FATAL_ERROR "usage: cmake -DEXE=<exe> [-DBASE=<filter>] -DPARTS=<f>|<f>... -P split_check.cmake")
endif()

# The case names a run with these doctest arguments would run, sorted. CMake lists split on ';'
# and treat brackets specially: those are replaced in the names first (some names have a ';').
function(list_cases out)
  execute_process(COMMAND "${EXE}" --list-test-cases --no-intro=true ${ARGN}
                  OUTPUT_VARIABLE text ERROR_VARIABLE err RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${EXE} --list-test-cases ${ARGN}: exit ${rc}\n${err}")
  endif()
  string(REPLACE ";" "<semicolon>" text "${text}")
  string(REPLACE "[" "<lb>" text "${text}")
  string(REPLACE "]" "<rb>" text "${text}")
  string(REPLACE "\r" "" text "${text}")
  string(REPLACE "\n" ";" lines "${text}")
  set(names "")
  foreach(l IN LISTS lines)
    if(l STREQUAL "" OR l MATCHES "^<lb>doctest<rb>" OR l MATCHES "^====")
      continue()
    endif()
    list(APPEND names "${l}")
  endforeach()
  list(SORT names)
  set(${out} "${names}" PARENT_SCOPE)
endfunction()

set(base_args "")
if(BASE)
  set(base_args "--test-case=${BASE}")
endif()
list_cases(all ${base_args})
list(LENGTH all n_all)

string(REPLACE "|" ";" part_filters "${PARTS}")
set(seen "")
set(excludes "")
foreach(f IN LISTS part_filters)
  list_cases(part "--test-case=${f}")  # as ctest runs it (a case outside BASE fails below)
  list(LENGTH part n)
  if(n EQUAL 0)
    message(FATAL_ERROR "part \"${f}\" matches no test case")
  endif()
  message(STATUS "part \"${f}\": ${n} cases")
  list(APPEND seen ${part})
  if(excludes STREQUAL "")
    set(excludes "${f}")
  else()
    set(excludes "${excludes},${f}")
  endif()
endforeach()
list_cases(rest ${base_args} "--test-case-exclude=${excludes}")
list(LENGTH rest n_rest)
message(STATUS "rest: ${n_rest} cases")
list(APPEND seen ${rest})
list(SORT seen)
list(LENGTH seen n_seen)

if(NOT seen STREQUAL all)
  set(dups "${seen}")
  list(REMOVE_DUPLICATES dups)
  set(missing "${all}")
  if(dups)
    list(REMOVE_ITEM missing ${dups})
  endif()
  message(FATAL_ERROR "the split runs ${n_seen} cases, the executable ${n_all}\n"
                      "missing: ${missing}\nlisted: ${seen}")
endif()
message(STATUS "OK: ${n_all} cases, each in exactly one ctest entry")
