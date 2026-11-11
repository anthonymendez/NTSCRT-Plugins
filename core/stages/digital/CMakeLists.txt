# Owner: agent C. The Digital video stage (port of upstream Sources/CrtCodec).
#
# Built as an object library linked into ntscrt_core so it can have its own
# flags: upstream forces -O for this target even in Debug (the codec must stay
# usable while debugging the rest), and FP contraction is off so results don't
# depend on whether the compiler fuses multiply-adds (Swift doesn't).
add_library(ntscrt_digital OBJECT
  ${CMAKE_CURRENT_SOURCE_DIR}/digital.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/renderer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/codec.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/video1.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/parallel.cpp)
target_include_directories(ntscrt_digital PRIVATE
  ${NTSCRT_ROOT}/core ${NTSCRT_ROOT}/core/src ${NTSCRT_ROOT}/core/include)
target_link_libraries(ntscrt_digital PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(ntscrt_digital PRIVATE NTSCRT_BUILDING)
find_package(Threads REQUIRED)

if(MSVC)
  # /RTC1 (in the Debug flags) can't be combined with /O2; drop it for the
  # targets of this directory only. _DEBUG, the debug CRT and iterator
  # debugging stay, so the objects remain ABI-compatible with Debug.
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
  target_compile_options(ntscrt_digital PRIVATE "$<$<CONFIG:Debug>:/O2;/Ob2>" /fp:precise)
else()
  target_compile_options(ntscrt_digital PRIVATE $<$<CONFIG:Debug>:-O2> -ffp-contract=off)
endif()

target_link_libraries(ntscrt_core PRIVATE ntscrt_digital Threads::Threads)
