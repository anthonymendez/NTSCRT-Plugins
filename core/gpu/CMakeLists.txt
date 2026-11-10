# Owner: agent D. Replace device_stub.cpp with d3d11/ (WIN32) and metal/ (APPLE) backends.
target_sources(ntscrt_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/device_stub.cpp)
