// ntscrt-warm — builds the NTSCRT CRT shaders into the user's shader cache, for the
// installers (install.ps1, the MSI, install.sh / the .pkg). Ships in the OpenFX bundle
// next to the plug-in binary (Contents/Win64/ntscrt-warm.exe, Contents/MacOS/ntscrt-warm)
// and loads that binary to call its ntscrt_ofx_warm_shader_cache: the same code, cache
// folder, key and preset files as a render in the host, so the host's first render of
// each preset finds its shaders. Run it as the user who renders (the cache is per user).
//
//   ntscrt-warm [--plugin <NTSCRT.ofx>] [--timeout <seconds, default 300>]
//
// Prints one line per preset. Exit codes: 0 every preset is in the cache, 1 nothing was
// built (the cache is off or can't be written, no GPU, librashader missing, root/SYSTEM),
// 2 some presets failed, 3 stopped at the time limit, 4 the plug-in couldn't be loaded,
// 5 bad arguments. Installers ignore it: a warm-up never fails an install (the plug-in
// builds whatever is missing in the background, or on first use).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace {

using progress_fn = void (*)(void *, const char *);
using warm_fn = int32_t (*)(progress_fn, void *);

std::mutex out_mu;
void line(const char *s) {
    std::lock_guard<std::mutex> lock(out_mu);
    std::printf("%s\n", s);
    std::fflush(stdout);
}
void progress(void *, const char *s) { line(s); }

[[noreturn]] void quit(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    // Without unloading the plug-in under a thread that may still be in it (time limit).
    TerminateProcess(GetCurrentProcess(), UINT(code));
#endif
    std::_Exit(code);
}

#ifdef _WIN32
// LocalSystem? (an MSI's elevated custom action): its cache would be nobody's.
bool running_as_system() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    alignas(TOKEN_USER) unsigned char buf[256];
    DWORD n = 0;
    bool system = false;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &n)) {
        PSID sys = nullptr;
        SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
        if (AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &sys)) {
            system = EqualSid(reinterpret_cast<TOKEN_USER *>(buf)->User.Sid, sys) != FALSE;
            FreeSid(sys);
        }
    }
    CloseHandle(tok);
    return system;
}
// The plug-in finds the cache through LOCALAPPDATA; an installer's custom action may run
// this with another account's environment. Point it at this user's own folder.
void use_own_local_appdata() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path)
        SetEnvironmentVariableW(L"LOCALAPPDATA", path);
    CoTaskMemFree(path);
}
std::wstring widen(const char *s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? size_t(n) : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
std::wstring default_plugin() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    const size_t slash = buf.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : buf.substr(0, slash + 1)) + L"NTSCRT.ofx";
}
#else
std::string default_plugin() {
    std::string exe;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    exe.resize(size);
    if (_NSGetExecutablePath(exe.data(), &size) != 0) exe.clear();
    exe = exe.c_str();
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) exe.assign(buf, size_t(n));
#endif
    const size_t slash = exe.find_last_of('/');
    return (slash == std::string::npos ? std::string() : exe.substr(0, slash + 1)) + "NTSCRT.ofx";
}
#endif

}  // namespace

int main(int argc, char **argv) {
    double timeout_s = 300;
    const char *plugin_arg = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--timeout") && i + 1 < argc) {
            timeout_s = std::atof(argv[++i]);
        } else if (!std::strcmp(argv[i], "--plugin") && i + 1 < argc) {
            plugin_arg = argv[++i];
        } else {
            std::fprintf(stderr, "usage: ntscrt-warm [--plugin <NTSCRT.ofx>] [--timeout <seconds>]\n");
            return 5;
        }
    }
    if (!(timeout_s > 0)) timeout_s = 300;
#ifdef _WIN32
    if (running_as_system()) {
        line("skipped: running as SYSTEM; run it as the user who renders (the shader cache is per user)");
        return 1;
    }
    use_own_local_appdata();  // before the plug-in loads (its C runtime copies the environment)
#else
    if (geteuid() == 0) {
        line("skipped: running as root; run it as the user who renders (the shader cache is per user)");
        return 1;
    }
#endif

#ifdef _WIN32
    const std::wstring plugin = plugin_arg ? widen(plugin_arg) : default_plugin();
    // The plug-in's own folder for its dependencies, as a host loads it.
    HMODULE h = LoadLibraryExW(plugin.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "couldn't load the plug-in (error %lu)", GetLastError());
        line(msg);
        return 4;
    }
    auto warm = reinterpret_cast<warm_fn>(GetProcAddress(h, "ntscrt_ofx_warm_shader_cache"));
#else
    const std::string plugin = plugin_arg ? plugin_arg : default_plugin();
    void *h = dlopen(plugin.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        line((std::string("couldn't load the plug-in: ") + (e ? e : "?")).c_str());
        return 4;
    }
    auto warm = reinterpret_cast<warm_fn>(dlsym(h, "ntscrt_ofx_warm_shader_cache"));
#endif
    if (!warm) {
        line("this plug-in version has no shader warm-up");
        return 4;
    }

    // On a thread, so a GPU driver that hangs can't hold the installer past the limit.
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    int status = 99;
    std::thread th([&] {
        const int s = warm(&progress, nullptr);
        std::lock_guard<std::mutex> lock(mu);
        status = s;
        done = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mu);
        if (!cv.wait_for(lock, std::chrono::duration<double>(timeout_s), [&] { return done; })) {
            char msg[160];
            std::snprintf(msg, sizeof msg,
                          "stopped after %g s; the plug-in builds the rest in the background when first used",
                          timeout_s);
            line(msg);
            quit(3);
        }
    }
    th.join();
    // 0 NTSCRT_OK, -1 cache off, 3 NTSCRT_ERR_GPU, 4 NTSCRT_ERR_MISSING_RESOURCE, else failures.
    quit(status == 0 ? 0 : (status == -1 || status == 3 || status == 4) ? 1 : 2);
}
