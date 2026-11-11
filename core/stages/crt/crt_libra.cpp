// crt_libra.cpp — runtime loading of librashader (portable part).
#include "crt_libra.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <librashader.h>  // core types only; runtime functions live in the backend files

namespace fs = std::filesystem;

namespace {
inline std::string u8str(const std::filesystem::path &p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
}  // namespace

namespace ntscrt::crt {

namespace {

#if defined(_WIN32)
const char *kLibName = "librashader.dll";
#elif defined(__APPLE__)
const char *kLibName = "librashader.dylib";
#else
const char *kLibName = "librashader.so";
#endif

fs::path module_dir() {
#ifdef _WIN32
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_dir), &m))
        return {};
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(m, buf, DWORD(std::size(buf)));
    if (!n) return {};
    return fs::path(std::wstring(buf, n)).parent_path();
#else
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&module_dir), &info) || !info.dli_fname) return {};
    return fs::path(info.dli_fname).parent_path();
#endif
}

void *open_lib(const fs::path &p) {
#ifdef _WIN32
    // Search the DLL's own directory for its dependencies.
    return LoadLibraryExW(p.wstring().c_str(), nullptr,
                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    return dlopen(p.string().c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
}

std::string last_os_error() {
#ifdef _WIN32
    return "error " + std::to_string(GetLastError());
#else
    const char *e = dlerror();
    return e ? e : "unknown";
#endif
}

}  // namespace

void *Library::sym(const char *name) const {
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

Library *Library::load(const std::string &resource_dir, std::string &err) {
    static std::mutex mu;
    static std::unique_ptr<Library> lib;
    std::lock_guard<std::mutex> lock(mu);
    if (lib) return lib.get();

    std::vector<fs::path> cand;
    if (const char *env = std::getenv("NTSCRT_LIBRASHADER"); env && *env) cand.push_back(fs::u8path(env));
    if (!resource_dir.empty()) {
        fs::path r = fs::u8path(resource_dir);
        cand.push_back(r / kLibName);
        cand.push_back(r / ".." / "Frameworks" / kLibName);
        cand.push_back(r / ".." / kLibName);
    }
    if (auto m = module_dir(); !m.empty()) cand.push_back(m / kLibName);

    std::string tried;
    for (const auto &p : cand) {
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            tried += "\n  " + u8str(p) + " (missing)";
            continue;
        }
        void *h = open_lib(p);
        if (!h) {
            tried += "\n  " + u8str(p) + " (load failed: " + last_os_error() + ")";
            continue;
        }
        auto l = std::make_unique<Library>();
        l->handle_ = h;
        l->path = u8str(p);
        auto abi = reinterpret_cast<PFN_libra_instance_abi_version>(l->sym("libra_instance_abi_version"));
        auto api = reinterpret_cast<PFN_libra_instance_api_version>(l->sym("libra_instance_api_version"));
        if (!abi || !api) {
            tried += "\n  " + u8str(p) + " (not librashader)";
            continue;
        }
        if (abi() != LIBRASHADER_CURRENT_ABI) {
            err = "librashader ABI mismatch in " + u8str(p) + ": have " + std::to_string(abi()) + ", expected " +
                  std::to_string(LIBRASHADER_CURRENT_ABI);
            return nullptr;
        }
        if (api() < LIBRASHADER_CURRENT_VERSION) {
            err = "librashader API too old in " + u8str(p);
            return nullptr;
        }
        lib = std::move(l);
        return lib.get();
    }
    err = std::string("librashader not found; tried:") + tried;
    return nullptr;
}

std::string Library::take_error(void *e) const {
    if (!e) return "unknown error";
    auto write = reinterpret_cast<PFN_libra_error_write>(sym("libra_error_write"));
    auto free_str = reinterpret_cast<PFN_libra_error_free_string>(sym("libra_error_free_string"));
    auto free_err = reinterpret_cast<PFN_libra_error_free>(sym("libra_error_free"));
    std::string out = "unknown librashader error";
    char *buf = nullptr;
    libra_error_t err = static_cast<libra_error_t>(e);
    if (write && write(err, &buf) == 0 && buf) out = buf;
    if (buf && free_str) free_str(&buf);
    if (free_err) free_err(&err);
    return out;
}

void *Library::open_preset(const std::string &preset_path, std::vector<ShaderParam> &params) {
    auto create = reinterpret_cast<PFN_libra_preset_create>(sym("libra_preset_create"));
    auto get_params = reinterpret_cast<PFN_libra_preset_get_runtime_params>(sym("libra_preset_get_runtime_params"));
    auto free_params = reinterpret_cast<PFN_libra_preset_free_runtime_params>(sym("libra_preset_free_runtime_params"));
    if (!create || !get_params || !free_params) throw std::runtime_error("librashader: preset functions missing");
    libra_shader_preset_t preset = nullptr;
    if (libra_error_t e = create(preset_path.c_str(), &preset))
        throw PresetError("librashader preset_create(" + preset_path + "): " + take_error(e));
    libra_preset_param_list_t list{};
    if (libra_error_t e = get_params(&preset, &list)) {
        free_preset(preset);
        throw std::runtime_error("librashader preset_get_runtime_params: " + take_error(e));
    }
    params.clear();
    for (uint64_t i = 0; i < list.length; ++i) {
        const auto &p = list.parameters[i];
        bool dup = false;  // upstream dedupeByName
        for (const auto &q : params) dup = dup || q.name == p.name;
        if (!dup) params.push_back({p.name ? p.name : "", p.initial, p.minimum, p.maximum, p.step});
    }
    free_params(list);
    return preset;
}

void Library::free_preset(void *preset) {
    auto f = reinterpret_cast<PFN_libra_preset_free>(sym("libra_preset_free"));
    libra_shader_preset_t p = static_cast<libra_shader_preset_t>(preset);
    if (f && p) f(&p);
}

std::unique_ptr<Chain> Library::create_chain(gpu::Device &dev, const std::string &preset_path) {
    if (dev.backend() == gpu::Backend::D3D11) return create_d3d11_chain(*this, dev, preset_path);
    return create_metal_chain(*this, dev, preset_path);
}

#ifndef _WIN32
std::unique_ptr<Chain> create_d3d11_chain(Library &, gpu::Device &, const std::string &) {
    throw std::runtime_error("librashader D3D11 runtime is Windows-only");
}
#endif
#ifndef __APPLE__
std::unique_ptr<Chain> create_metal_chain(Library &, gpu::Device &, const std::string &) {
    throw std::runtime_error("librashader Metal runtime is macOS-only");
}
#endif

}  // namespace ntscrt::crt
