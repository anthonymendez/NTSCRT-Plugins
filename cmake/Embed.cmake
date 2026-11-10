# ntscrt_embed_text(<out.cpp> <input> <c_function_name>)
# Generates a .cpp defining `extern "C" const char* <fn>(void)` that returns the
# file's bytes, NUL-terminated. Byte-array form avoids MSVC's string-literal limit.
function(ntscrt_embed_text out_cpp input fn)
  add_custom_command(
    OUTPUT ${out_cpp}
    COMMAND ${CMAKE_COMMAND} -DIN=${input} -DOUT=${out_cpp} -DFN=${fn} -P ${NTSCRT_ROOT}/cmake/EmbedRun.cmake
    DEPENDS ${input} ${NTSCRT_ROOT}/cmake/EmbedRun.cmake
    COMMENT "Embedding ${input}")
endfunction()
