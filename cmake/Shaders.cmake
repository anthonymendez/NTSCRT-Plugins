# ntscrt_add_shaders(<target> <file.slang>...)
# CONTRACT (Wave 0). Owner: agent D replaces this with the real version:
#   slangc -> HLSL + MSL text per file, embedded into <target>, looked up at
#   runtime by gpu::Device::kernel("<file stem>/<entry point>").
# Wave-0 stub: only records the list, so stage owners can register shaders now.
function(ntscrt_add_shaders target)
  set_property(GLOBAL APPEND PROPERTY NTSCRT_SHADER_FILES ${ARGN})
endfunction()
