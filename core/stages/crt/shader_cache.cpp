// shader_cache.cpp — librashader's shader cache folder (shader_cache.h).
#include "shader_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#include <librashader.h>

#include "crt_libra.h"

#ifndef NTSCRT_CACHE_VERSION
#define NTSCRT_CACHE_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace ntscrt::crt {

namespace {

std::string u8str(const fs::path &p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// "<size> <hash of the contents>" of a file, "?" if unreadable. The contents, not the
// file time: a reinstall or a copied bundle (the Resolve tests copy it) is the same
// library. Once per process, ~16 MB in all (librashader, d3dcompiler_47): ~10 ms.
std::string file_stamp(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "?";
    uint64_t h = 1469598103934665603ull, size = 0;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        const size_t n = size_t(f.gcount());
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t w;
            std::memcpy(&w, buf.data() + i, 8);
            h = (h ^ w) * 1099511628211ull;
            h ^= h >> 29;
        }
        for (; i < n; ++i) h = (h ^ static_cast<unsigned char>(buf[i])) * 1099511628211ull;
        size += n;
    }
    char out[48];
    std::snprintf(out, sizeof out, "%llu %016llx", static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(h));
    return out;
}

uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

uint64_t cap_bytes() {
    const char *e = std::getenv("NTSCRT_SHADER_CACHE_MB");
    long long mb = e && *e ? std::atoll(e) : 256;
    if (mb < 1) mb = 256;
    return uint64_t(mb) << 20;
}

bool starts_with(const std::string &s, std::string_view p) { return s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string &s, std::string_view p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

unsigned long process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Writes `text` to `p` through a temporary file in the same folder (never a half
// file under the real name). false if the folder isn't writable.
bool write_atomic(const fs::path &p, const std::string &text) {
    static std::atomic<unsigned> seq{0};
    fs::path tmp = p;
    tmp += "." + std::to_string(process_id()) + "-" + std::to_string(seq++) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        f.close();
        if (!f) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        return fs::exists(p, ec2);  // another process just wrote it
    }
    return true;
}

}  // namespace

fs::path shader_cache_root(std::string &why) {
    const char *e = std::getenv("NTSCRT_SHADER_CACHE");
    const std::string v = e ? e : "";
    if (v == "0" || v == "off" || v == "OFF" || v == "false") {
        why = "NTSCRT_SHADER_CACHE=" + v;
        return {};
    }
    if (!v.empty() && v != "1" && v != "on" && v != "ON") return fs::u8path(v);
#ifdef _WIN32
    const char *base = std::getenv("LOCALAPPDATA");
    if (base && *base) return fs::u8path(base) / "NTSCRT" / "cache";
    why = "LOCALAPPDATA unset";
#elif defined(__APPLE__)
    const char *home = std::getenv("HOME");
    if (home && *home) return fs::u8path(home) / "Library" / "Caches" / "NTSCRT";
    why = "HOME unset";
#else
    const char *xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return fs::u8path(xdg) / "ntscrt";
    const char *home = std::getenv("HOME");
    if (home && *home) return fs::u8path(home) / ".cache" / "ntscrt";
    why = "HOME unset";
#endif
    return {};
}

std::string shader_cache_key_text(Library &lib, gpu::Device &dev) {
    std::ostringstream k;
    k << "NTSCRT " << NTSCRT_CACHE_VERSION << "\n";
    auto abi = reinterpret_cast<PFN_libra_instance_abi_version>(lib.sym("libra_instance_abi_version"));
    auto api = reinterpret_cast<PFN_libra_instance_api_version>(lib.sym("libra_instance_api_version"));
    k << "librashader abi " << (abi ? uint64_t(abi()) : 0) << " api " << (api ? uint64_t(api()) : 0) << " file "
      << file_stamp(fs::u8path(lib.path)) << "\n";
#ifdef _WIN32
    // The HLSL -> DXBC compiler librashader uses (Windows' own, updated with it).
    if (HMODULE m = GetModuleHandleW(L"d3dcompiler_47.dll")) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetModuleFileNameW(m, buf, MAX_PATH);
        if (n && n < MAX_PATH) k << "d3dcompiler " << file_stamp(fs::path(std::wstring(buf, n))) << "\n";
    }
    if (dev.backend() == gpu::Backend::D3D11) {
        auto *d = static_cast<ID3D11Device *>(dev.native_device());
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
        DXGI_ADAPTER_DESC desc{};
        if (d && SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&ad)) &&
            SUCCEEDED(ad->GetDesc(&desc))) {
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, int(sizeof name) - 1, nullptr, nullptr);
            char ids[96];
            std::snprintf(ids, sizeof ids, "%04x:%04x:%08x:%02x", desc.VendorId, desc.DeviceId, desc.SubSysId,
                          desc.Revision);
            k << "gpu " << ids << " " << name;
            LARGE_INTEGER umd{};
            if (SUCCEEDED(ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
                const uint64_t v = uint64_t(umd.QuadPart);
                k << " driver " << (v >> 48) << "." << ((v >> 32) & 0xffff) << "." << ((v >> 16) & 0xffff) << "."
                  << (v & 0xffff);
            }
            k << "\n";
        }
    }
#else
    (void)dev;
    struct utsname u {};
    if (uname(&u) == 0) k << "os " << u.sysname << " " << u.release << " " << u.machine << "\n";
#endif
    return k.str();
}

std::string shader_cache_folder_name(const std::string &key_text) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "shaders-%016llx", static_cast<unsigned long long>(fnv1a64(key_text)));
    return buf;
}

void trim_shader_cache(const fs::path &root, const fs::path &keep, uint64_t cap, int max_age_days) {
    using namespace std::chrono;
    const auto now = fs::file_time_type::clock::now();
    struct File {
        fs::path path;
        fs::file_time_type time;
        uint64_t size;
    };
    std::vector<File> files;
    uint64_t total = 0;
    std::error_code ec;
    for (fs::directory_iterator d(root, ec), end; !ec && d != end; d.increment(ec)) {
        std::error_code e1;
        const std::string dname = u8str(d->path().filename());
        if (!starts_with(dname, "shaders-") || !d->is_directory(e1)) continue;  // not ours
        const bool current = d->path() == keep;
        std::vector<File> mine;
        // Last used: its newest file (reads touch them), else the folder's own time.
        fs::file_time_type newest = fs::file_time_type::min();
        bool any = false;
        std::error_code e2;
        for (fs::directory_iterator f(d->path(), e2), fend; !e2 && f != fend; f.increment(e2)) {
            std::error_code e3;
            const std::string name = u8str(f->path().filename());
            if (!f->is_regular_file(e3)) continue;
            const auto t = f->last_write_time(e3);
            if (e3) continue;
            if (ends_with(name, ".tmp")) {  // left by a process that died while writing
                if (now - t > hours(1)) fs::remove(f->path(), e3);
                continue;
            }
            newest = std::max(newest, t);
            any = true;
            if (!ends_with(name, ".bin")) continue;
            const uint64_t size = f->file_size(e3);
            if (!e3) mine.push_back({f->path(), t, size});
        }
        if (!any) {
            newest = d->last_write_time(e1);
            if (e1) newest = now;
        }
        if (!current && now - newest > hours(24) * max_age_days) {
            // Unused for a week (another plug-in version, GPU or driver): only our own
            // files, then the folder if that emptied it.
            for (const File &f : mine) fs::remove(f.path, e1);
            fs::remove(d->path() / "key.txt", e1);
            fs::remove(d->path(), e1);
            continue;
        }
        for (File &f : mine) {
            total += f.size;
            files.push_back(std::move(f));
        }
    }
    if (total <= cap) return;
    std::sort(files.begin(), files.end(), [](const File &a, const File &b) { return a.time < b.time; });
    for (const File &f : files) {
        if (total <= cap / 4 * 3) break;
        std::error_code e;
        if (fs::remove(f.path, e)) total -= std::min(total, f.size);
    }
}

const ShaderCacheState &configure_shader_cache(Library &lib, gpu::Device &dev) {
    static std::mutex mu;
    static ShaderCacheState state;
    static bool done = false;
    std::lock_guard<std::mutex> lock(mu);
    if (done) return state;
    done = true;
    try {
        auto set_dir = reinterpret_cast<int32_t (*)(const char *)>(lib.sym("ntscrt_libra_set_cache_dir"));
        if (!set_dir) {
            state.why = "librashader without NTSCRT's cache (" + lib.path + ")";
            return state;
        }
        std::string why;
        fs::path root = shader_cache_root(why);
        if (root.empty()) {
            state.why = why;
            return state;
        }
        const std::string key = shader_cache_key_text(lib, dev);
        const fs::path dir = root / shader_cache_folder_name(key);
        std::error_code ec;
        fs::create_directories(dir, ec);
        // key.txt says what the folder is for; writing it also proves the folder writable.
        if (!fs::exists(dir / "key.txt", ec) && !write_atomic(dir / "key.txt", key)) {
            state.why = "can't write to " + u8str(dir);
            return state;
        }
        if (!fs::is_directory(dir, ec)) {
            state.why = "can't create " + u8str(dir);
            return state;
        }
        trim_shader_cache(root, dir, cap_bytes());
        state.dir = u8str(dir);
        set_dir(state.dir.c_str());
        state.on = true;
    } catch (const std::exception &e) {  // never a reason to fail a frame
        state = ShaderCacheState{};
        state.why = e.what();
    }
    return state;
}

}  // namespace ntscrt::crt
