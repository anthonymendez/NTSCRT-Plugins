// shader_warm.cpp — librashader's shader cache, filled ahead of time (shader_warm.h).
#include "shader_warm.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include "../stages.h"
#include "crt_libra.h"
#include "shader_cache.h"

namespace fs = std::filesystem;

namespace ntscrt::crt {

namespace {

std::string u8str(const fs::path &p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Runs on the isolated pool's thread (ntscrt_libra_run_isolated): lowest CPU (and on
// Windows I/O) priority for that thread, once.
void lower_this_thread() {
    thread_local bool done = false;
    if (done) return;
    done = true;
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
}

struct Job {
    Library *lib;
    gpu::Device *dev;
    std::string path;
    bool isolated;
    std::exception_ptr error;
};

void build(void *p) noexcept {
    Job &j = *static_cast<Job *>(p);
    try {
        if (j.isolated) lower_this_thread();
        j.lib->create_chain(*j.dev, j.path, /*compile_only=*/true);  // built, cached, freed
    } catch (...) {
        j.error = std::current_exception();
    }
}

}  // namespace

WarmResult warm_shader_cache(gpu::Device &dev, const std::string &resource_dir, const WarmOptions &opt) {
    WarmResult r;
    auto say = [&](const std::string &s) {
        if (opt.progress) opt.progress(s);
    };
    const auto t0 = std::chrono::steady_clock::now();
    std::string err;
    Library *lib = Library::load(resource_dir, err);
    if (!lib) {
        r.why = err;
        return r;
    }
    const ShaderCacheState &cache = configure_shader_cache(*lib, dev);
    if (!cache.on) {
        r.why = "shader cache off: " + cache.why;
        return r;
    }
    r.cache_on = true;
    r.dir = cache.dir;
    const auto chains = crt_bundled_chain_files();
    std::string list;
    for (const auto &c : chains) list += (list.empty() ? "" : " ") + c.second;
    const fs::path dir = fs::u8path(cache.dir);
    if (opt.skip_if_complete && shader_cache_warm_complete(dir, list)) {
        r.already_complete = true;
        return r;
    }
    using run_fn = int32_t (*)(void (*)(void *), void *);
    auto run_isolated = reinterpret_cast<run_fn>(lib->sym("ntscrt_libra_run_isolated"));
    r.presets = int(chains.size());
    for (const auto &[id, rel] : chains) {
        if (opt.before_preset && !opt.before_preset()) {
            r.stopped = true;
            break;
        }
        const fs::path p = fs::u8path(resource_dir) / "shaders" / fs::u8path(rel);
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            ++r.failed;
            say(id + ": missing " + u8str(p));
            continue;
        }
        uint64_t h0, m0, w0, h1, m1, w1;
        lib->shader_cache_stats(h0, m0, w0);
        const auto s0 = std::chrono::steady_clock::now();
        Job job{lib, &dev, u8str(p), opt.isolated && run_isolated, nullptr};
        if (job.isolated) {
            if (run_isolated(&build, &job) != 0) {  // no pool: on this thread
                job.isolated = false;
                build(&job);
            }
        } else {
            build(&job);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
        lib->shader_cache_stats(h1, m1, w1);  // process-wide: renders meanwhile count too
        char line[160];
        if (job.error) {
            ++r.failed;
            std::string what = "unknown error";
            try {
                std::rethrow_exception(job.error);
            } catch (const std::exception &e) {
                what = e.what();
            } catch (...) {
            }
            if (dev.lost()) r.why = "the GPU device was lost";
            say(id + ": failed: " + what);
            if (dev.lost()) break;
            continue;
        }
        ++r.built;
        r.found += h1 - h0;
        r.compiled += m1 - m0;
        std::snprintf(line, sizeof line, "%s: %ld ms (cache found %llu compiled %llu)", id.c_str(), long(ms + 0.5),
                      static_cast<unsigned long long>(h1 - h0), static_cast<unsigned long long>(m1 - m0));
        say(line);
    }
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!r.stopped && r.failed == 0 && r.built == r.presets) mark_shader_cache_warm(dir, list);
    return r;
}

}  // namespace ntscrt::crt
