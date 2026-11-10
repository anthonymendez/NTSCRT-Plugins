# Owner: agent D. GPU HAL: d3d11/ (WIN32), metal/ (APPLE), shader registry.
target_sources(ntscrt_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/device.cpp)
ntscrt_add_shaders(ntscrt_core ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gpu_builtin.slang)

if(WIN32)
  target_sources(ntscrt_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/d3d11/d3d11_device.cpp)
  # d3dcompiler_47.dll is loaded at runtime (LoadLibrary), not linked.
  target_link_libraries(ntscrt_core PUBLIC d3d11 dxgi)
endif()

if(APPLE)
  set(mm ${CMAKE_CURRENT_SOURCE_DIR}/metal/metal_device.mm)
  target_sources(ntscrt_core PRIVATE ${mm})
  set_source_files_properties(${mm} TARGET_DIRECTORY ntscrt_core PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(ntscrt_core PUBLIC "-framework Metal" "-framework Foundation")
endif()
