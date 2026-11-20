// shader_cache.h — where librashader keeps the CRT shaders it compiled (SPIR-V and,
// on Windows, DXBC), so a preset's first frame in a later session doesn't compile
// them again. Private to core/stages/crt.
//
// The folder (NTSCRT_SHADER_CACHE; docs/ENVIRONMENT.md):
//   Windows %LOCALAPPDATA%\NTSCRT\cache, macOS ~/Library/Caches/NTSCRT,
//   else $XDG_CACHE_HOME/ntscrt (~/.cache/ntscrt).
// Inside it, one sub-folder per key, "shaders-<hash>", the hash of: the plug-in
// version, the librashader library (ABI / API version, file size and contents) and, on
// Windows, d3dcompiler_47.dll and the GPU and its driver version; macOS: the OS
// release (the cached SPIR-V doesn't depend on the GPU there). Any of them changing
// starts an empty folder; folders not used for 7 days are removed, and the files
// least recently used go first when all of them pass NTSCRT_SHADER_CACHE_MB (256).
// librashader (as patched by rust/librashader-patch) writes each object to a
// temporary file and renames it, and checks a hash when reading, so several Resolve
// processes and effects can share the folder; anything going wrong (a read-only or
// missing folder, a damaged file) just means compiling, as without a cache.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "../../gpu/gpu.h"

namespace ntscrt::crt {

class Library;

struct ShaderCacheState {
    bool on = false;
    std::string dir;  // the key folder (UTF-8) when on
    std::string why;  // when off: why (log)
};

// Sets up librashader's cache for this process, once (later calls return the first
// result): folder from the environment, key from `lib` and `dev`, a trim of old
// folders and files, then the folder handed to librashader.
const ShaderCacheState &configure_shader_cache(Library &lib, gpu::Device &dev);

// Parts, for tests.
// The cache root from NTSCRT_SHADER_CACHE / the default; empty (with `why`) when off.
std::filesystem::path shader_cache_root(std::string &why);
// The text the key folder's name hashes (also written to its key.txt).
std::string shader_cache_key_text(Library &lib, gpu::Device &dev);
std::string shader_cache_folder_name(const std::string &key_text);
// Removes stale temporary files, "shaders-*" folders other than `keep` unused for
// `max_age_days`, then the least recently used *.bin files of all of them until
// they take at most 3/4 of `cap_bytes` (when over `cap_bytes`). Touches nothing
// else under `root`. Errors are ignored.
void trim_shader_cache(const std::filesystem::path &root, const std::filesystem::path &keep, uint64_t cap_bytes,
                       int max_age_days = 7);

}  // namespace ntscrt::crt
