# apply.cmake — builds the patched librashader source tree the plug-ins' librashader
# library is compiled from (core/stages/crt/CMakeLists.txt runs it before cargo):
#
#   cmake -DSRC=<vendor/librashader> -DDST=<build>/librashader-src -DPATCH=<this dir> -P apply.cmake
#
# Copies the pinned submodule (vendor/librashader, left untouched) to DST and changes it:
# 1. librashader-cache keeps its compiled shaders (SPIR-V, DXBC) in files in a folder the
#    plug-in chooses, written atomically and checked when read (store.rs, see its header),
#    instead of its persy database in %LOCALAPPDATA%\librashader; nothing is cached until
#    the plug-in calls ntscrt_libra_set_cache_dir (core/stages/crt/shader_cache.cpp).
# 2. The Metal runtime caches its SPIR-V like the Direct3D 11 one (it had no cache).
# 3. ntscrt_libra_run_isolated (pool.rs): a chain built through it compiles on a private
#    one-thread rayon pool, not the global one renders use (the background warm-up's).
# Every edit must match the source exactly once, or the build stops: after a librashader
# update, check them against the new code. The changed files stay under librashader's
# licence (MPL-2.0 OR GPL-3.0-only); THIRD_PARTY_NOTICES.md says what changed.
foreach(v SRC DST PATCH)
  if(NOT DEFINED ${v})
    message(FATAL_ERROR "apply.cmake: -D${v}= missing")
  endif()
endforeach()

file(REMOVE_RECURSE "${DST}")
file(COPY "${SRC}/" DESTINATION "${DST}" PATTERN ".git" EXCLUDE)

# Replaces the one occurrence of `old` in DST/rel by `new` (line endings as LF).
function(ntscrt_patch rel old new)
  set(f "${DST}/${rel}")
  file(READ "${f}" s)
  string(REPLACE "\r\n" "\n" s "${s}")
  string(FIND "${s}" "${old}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "librashader patch: text not found in ${rel}:\n${old}")
  endif()
  string(LENGTH "${old}" n)
  math(EXPR after "${at} + ${n}")
  string(SUBSTRING "${s}" ${after} -1 rest)
  string(FIND "${rest}" "${old}" again)
  if(NOT again EQUAL -1)
    message(FATAL_ERROR "librashader patch: text found more than once in ${rel}:\n${old}")
  endif()
  string(SUBSTRING "${s}" 0 ${at} head)
  file(WRITE "${f}" "${head}${new}${rest}")
endfunction()

# ---- 1. librashader-cache: file store ----
file(COPY "${PATCH}/store.rs" "${PATCH}/pool.rs" DESTINATION "${DST}/librashader-cache/src")
ntscrt_patch(librashader-cache/src/lib.rs [=[
mod cache;
]=] [=[
mod cache;
mod pool;
mod store;

pub use store::set_cache_dir;
]=])
ntscrt_patch(librashader-cache/src/cache.rs [=[
use std::panic::{catch_unwind, AssertUnwindSafe};

pub(crate) mod internal {
]=] [=[
use std::panic::{catch_unwind, AssertUnwindSafe};

// NTSCRT-Plugins: the cache's storage is crate::store (the persy database below is unused).
pub(crate) use crate::store as internal;

#[allow(dead_code)]
pub(crate) mod internal_persy {
]=])

# Upstream writes a SPIR-V object back after reading it from the cache too: not again.
ntscrt_patch(librashader-cache/src/compilation.rs [=[
                    break 'cached compilation;
]=] [=[
                    return Ok(compilation); // NTSCRT-Plugins: found, nothing to write
]=])
ntscrt_patch(librashader-cache/src/compilation.rs [=[
        let compilation = 'cached: {
]=] [=[
        let compilation = {
]=])

# ---- 2. Metal runtime: SPIR-V through the cache ----
ntscrt_patch(librashader-cache/src/compilation.rs [=[
use librashader_reflect::back::targets::{GLSL, HLSL, SPIRV};
]=] [=[
use librashader_reflect::back::targets::{GLSL, HLSL, MSL, SPIRV};
]=])
file(APPEND "${DST}/librashader-cache/src/compilation.rs" [=[

// NTSCRT-Plugins: for the Metal runtime (librashader-runtime-mtl's compile_passes).
impl<T> FromCompilation<CachedCompilation<SpirvCompilation>, T> for MSL
where
    MSL: FromCompilation<SpirvCompilation, T>,
{
    type Target = <MSL as FromCompilation<SpirvCompilation, T>>::Target;
    type Options = <MSL as FromCompilation<SpirvCompilation, T>>::Options;
    type Context = <MSL as FromCompilation<SpirvCompilation, T>>::Context;
    type Output = <MSL as FromCompilation<SpirvCompilation, T>>::Output;

    fn from_compilation(
        compile: CachedCompilation<SpirvCompilation>,
    ) -> Result<CompilerBackend<Self::Output>, ShaderReflectError> {
        MSL::from_compilation(compile.compilation)
    }
}
]=])
ntscrt_patch(librashader-runtime-mtl/Cargo.toml [=[
librashader-runtime = { path = "../librashader-runtime" , version = "0.10.1" }
]=] [=[
librashader-runtime = { path = "../librashader-runtime" , version = "0.10.1" }
librashader-cache = { path = "../librashader-cache", version = "0.10.1" }
]=])
ntscrt_patch(Cargo.lock [=[
name = "librashader-runtime-mtl"
version = "0.10.1"
dependencies = [
 "array-concat",
 "bytemuck",
]=] [=[
name = "librashader-runtime-mtl"
version = "0.10.1"
dependencies = [
 "array-concat",
 "bytemuck",
 "librashader-cache",
]=])
ntscrt_patch(librashader-runtime-mtl/src/filter_chain.rs [=[
        let (passes, semantics) = MSL::compile_preset_passes::<
            SpirvCompilation,
            SpirvCross,
            FilterChainError,
        >(shaders, textures.iter().map(|t| &t.meta))?;
]=] [=[
        // NTSCRT-Plugins: cached like the D3D11 runtime's (nothing is cached until the
        // host sets a cache folder).
        let (passes, semantics) = MSL::compile_preset_passes::<
            librashader_cache::CachedCompilation<SpirvCompilation>,
            SpirvCross,
            FilterChainError,
        >(shaders, textures.iter().map(|t| &t.meta))?;
]=])

# ---- 3. a private rayon pool (pool.rs) ----
ntscrt_patch(librashader-cache/Cargo.toml [=[
persy = "1.6.0"
]=] [=[
persy = "1.6.0"
rayon = { workspace = true }
]=])
ntscrt_patch(Cargo.lock [=[
 "persy",
 "platform-dirs",
 "serde",
 "thiserror 2.0.18",
 "windows",
]
]=] [=[
 "persy",
 "platform-dirs",
 "rayon",
 "serde",
 "thiserror 2.0.18",
 "windows",
]
]=])

file(WRITE "${DST}/NTSCRT-PATCHED.txt"
  "Copy of vendor/librashader changed by rust/librashader-patch/apply.cmake. Do not edit: rebuilt from those.\n")
