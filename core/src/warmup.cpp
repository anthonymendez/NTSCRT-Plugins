// warmup.cpp — ntscrt_warm_shader_cache and its background variant (warmup.h).
#include "warmup.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>

#include "../stages/crt/shader_warm.h"
#include "testing.h"

namespace ntscrt::warmup {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<int> active_renders{0};
std::atomic<int64_t> last_render_end{0};  // Clock ticks; 0 = none yet

int env_ms(const char *name, int def) {
    const char *e = std::getenv(name);
    if (!e || !*e) return def;
    const long v = std::strtol(e, nullptr, 10);
    return v < 0 ? def : int(v);
}

bool background_off() {
    const char *e = std::getenv("NTSCRT_SHADER_WARMUP");
    if (!e) return false;
    const std::string_view v(e);
    return v == "0" || v == "off" || v == "OFF" || v == "false";
}

// The one background warm-up of this process.
struct Background {
    std::mutex m;
    std::condition_variable cv;
    std::thread th;
    bool stop = false;
    bool running = false;
    bool finished = false;  // ran to the end (or had nothing to do): not again in this process
    testing::WarmupState state;
};
Background &bg() {
    static Background *b = new Background;  // never destroyed: nothing at static destruction
    return *b;
}

std::string summary(const crt::WarmResult &r) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%d of %d presets ready in %.1f s: shader objects found %llu, compiled %llu",
                  r.built, r.presets, r.ms / 1000.0, static_cast<unsigned long long>(r.found),
                  static_cast<unsigned long long>(r.compiled));
    return std::string(buf) + (r.dir.empty() ? "" : " (" + r.dir + ")");
}

void run(std::shared_ptr<gpu::Device> dev, std::string resource_dir, ntscrt_warm_progress_fn progress, void *user) {
    Background &b = bg();
    auto say = [&](const std::string &s) {
        if (progress) progress(user, ("shader warm-up: " + s).c_str());
    };
    // Waits until `pred` or stop; false on stop.
    auto wait = [&](int ms) {
        std::unique_lock<std::mutex> lock(b.m);
        return !b.cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return b.stop; });
    };
    crt::WarmResult r;
    bool stopped = !wait(env_ms("NTSCRT_SHADER_WARMUP_DELAY_MS", 3000));
    if (!stopped) {
        const auto idle = std::chrono::milliseconds(env_ms("NTSCRT_SHADER_WARMUP_IDLE_MS", 1000));
        crt::WarmOptions o;
        o.isolated = true;
        o.skip_if_complete = true;
        o.progress = say;
        o.before_preset = [&] {
            // Only between renders: none running, none in the last `idle`.
            for (;;) {
                const int64_t end = last_render_end.load();
                if (active_renders.load() == 0 &&
                    (end == 0 || Clock::now() - Clock::time_point(Clock::duration(end)) >= idle))
                    return true;
                if (!wait(50)) return false;
            }
        };
        try {
            r = crt::warm_shader_cache(*dev, resource_dir, o);
        } catch (const std::exception &e) {  // never a reason to disturb the host
            r.why = e.what();
        } catch (...) {
            r.why = "unknown error";
        }
        stopped = r.stopped;
        if (r.already_complete) say("the cache already has every preset (" + r.dir + ")");
        else if (!r.why.empty() && r.built == 0) say("nothing to do: " + r.why);
        else if (!stopped) say(summary(r));
        else say("stopped (" + std::to_string(r.built) + " presets built): resumes with the next effect");
    }
    dev.reset();  // before saying it's done: the last user of the device may be waiting in stop_background
    std::lock_guard<std::mutex> lock(b.m);
    b.running = false;
    b.state.running = false;
    b.state.built += r.built;
    b.state.already_complete = r.already_complete;
    b.state.stopped = stopped;
    b.state.why = r.why;
    if (!stopped) {
        b.finished = true;
        ++b.state.completed;
    }
    b.cv.notify_all();
}

}  // namespace

RenderScope::RenderScope() { active_renders.fetch_add(1); }
RenderScope::~RenderScope() {
    last_render_end.store(Clock::now().time_since_epoch().count());
    active_renders.fetch_sub(1);
}

void start_background(std::shared_ptr<gpu::Device> dev, const std::string &resource_dir,
                      ntscrt_warm_progress_fn progress, void *user) {
    if (!dev || background_off()) return;
    Background &b = bg();
    std::thread old;
    {
        std::lock_guard<std::mutex> lock(b.m);
        if (b.finished || b.running) return;
        old = std::move(b.th);  // an earlier run that was stopped (it has ended: running is false)
        b.stop = false;
        b.running = true;
        b.state.running = true;
        ++b.state.runs;
        try {
            b.th = std::thread(run, std::move(dev), resource_dir, progress, user);
        } catch (...) {  // no thread: no warm-up
            b.running = false;
            b.state.running = false;
        }
    }
    if (old.joinable()) old.join();
}

void stop_background() {
    Background &b = bg();
    std::thread th;
    {
        std::lock_guard<std::mutex> lock(b.m);
        b.stop = true;
        th = std::move(b.th);
    }
    b.cv.notify_all();
    if (th.joinable() && th.get_id() != std::this_thread::get_id()) th.join();
}

}  // namespace ntscrt::warmup

namespace ntscrt::testing {
WarmupState warmup_state() {
    auto &b = warmup::bg();
    std::lock_guard<std::mutex> lock(b.m);
    return b.state;
}
bool wait_warmup(int timeout_ms) {
    auto &b = warmup::bg();
    std::unique_lock<std::mutex> lock(b.m);
    return b.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !b.running; });
}
void forget_warmup() {
    warmup::stop_background();
    auto &b = warmup::bg();
    std::lock_guard<std::mutex> lock(b.m);
    b.finished = false;
    b.state = WarmupState{};
}
}  // namespace ntscrt::testing

extern "C" {

ntscrt_status ntscrt_warm_shader_cache(const char *resource_dir, ntscrt_warm_progress_fn progress, void *user,
                                       ntscrt_warm_result *result) {
    using namespace ntscrt;
    auto say = [&](const std::string &s) {
        if (progress) progress(user, s.c_str());
    };
    if (result) *result = ntscrt_warm_result{};
    try {
#ifdef _WIN32
        const gpu::Backend backend = gpu::Backend::D3D11;
#else
        const gpu::Backend backend = gpu::Backend::Metal;
#endif
        std::shared_ptr<gpu::Device> dev = gpu::Device::shared(backend);
        if (!dev) {
            say(std::string("no GPU device: ") + gpu::Device::last_create_error());
            return NTSCRT_ERR_GPU;
        }
        crt::WarmOptions o;
        o.progress = say;
        const crt::WarmResult r = crt::warm_shader_cache(*dev, resource_dir ? resource_dir : "", o);
        if (result) {
            result->presets = r.presets;
            result->built = r.built;
            result->failed = r.failed;
            result->cache_on = r.cache_on ? 1 : 0;
            result->found = r.found;
            result->compiled = r.compiled;
            result->ms = r.ms;
        }
        if (!r.cache_on) {
            say(r.why);
            return r.why.rfind("shader cache off", 0) == 0 ? NTSCRT_OK : NTSCRT_ERR_MISSING_RESOURCE;
        }
        say(warmup::summary(r));
        if (r.failed == 0 && r.built == r.presets) return NTSCRT_OK;
        if (dev->lost()) return NTSCRT_ERR_GPU;
        return NTSCRT_ERR_INTERNAL;
    } catch (const std::exception &e) {
        say(std::string("failed: ") + e.what());
        return NTSCRT_ERR_INTERNAL;
    } catch (...) {
        say("failed");
        return NTSCRT_ERR_INTERNAL;
    }
}

}  // extern "C"
