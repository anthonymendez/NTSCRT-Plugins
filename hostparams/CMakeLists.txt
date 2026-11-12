# Host-independent parameter model + diagnostic log shared by the OpenFX and
# FxPlug plug-ins (no OFX / Apple headers). Headers: #include "ntscrt_host/params.h".
add_library(ntscrt_hostparams STATIC
  ntscrt_host/params.h
  ntscrt_host/file_log.h
  ntscrt_host/debug_switch.h
  src/params.cpp
  src/file_log.cpp
  src/debug_switch.cpp)
target_include_directories(ntscrt_hostparams PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${NTSCRT_ROOT}/core/include)
target_link_libraries(ntscrt_hostparams PUBLIC nlohmann_json::nlohmann_json)
set_target_properties(ntscrt_hostparams PROPERTIES POSITION_INDEPENDENT_CODE ON)
