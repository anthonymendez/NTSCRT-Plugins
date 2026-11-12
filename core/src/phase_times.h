// phase_times.h — where one render's time went (ntscrt_last_timings), for debug logs.
// NTSCRT_PROFILE=1 waits for the GPU at the end of each GPU phase so its work
// is counted there, not in whichever later phase waits for it (slower: profiling only).
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../gpu/gpu.h"

namespace ntscrt {

struct PhaseTimes {
    enum Phase { In, Digital, Ntsc, Upload, Downscale, Glitch, Crt, Out, kCount };
    const char *path = "cpu";
    double ms[kCount] = {};
    bool ran[kCount] = {};
    std::string text;
    std::string note;  // appended to text once (e.g. the host GPU bridge just made)

    void clear(const char *p) {
        path = p;
        note.clear();
        for (int i = 0; i < kCount; ++i) ms[i] = 0, ran[i] = false;
    }
    void finish() {
        static const char *names[kCount] = {"in", "digital", "ntsc", "upload", "downscale", "glitch", "crt", "out"};
        text = std::string("path=") + path;
        char b[48];
        for (int i = 0; i < kCount; ++i)
            if (ran[i]) {
                std::snprintf(b, sizeof b, " %s=%.2f", names[i], ms[i]);
                text += b;
            }
        text += note;
    }
    static bool profiling() {
        static const bool on = [] {
            const char *e = std::getenv("NTSCRT_PROFILE");
            return e && *e && *e != '0';
        }();
        return on;
    }
};

// Times a scope into `t.ms[phase]`; with a device and NTSCRT_PROFILE, waits for
// the GPU work queued in it first.
class PhaseScope {
public:
    PhaseScope(PhaseTimes &t, PhaseTimes::Phase p, gpu::Device *dev = nullptr)
        : t_(t), p_(p), dev_(dev), t0_(std::chrono::steady_clock::now()) {}
    ~PhaseScope() {
        if (dev_ && PhaseTimes::profiling()) {
            try {
                dev_->finish();
            } catch (...) {
            }
        }
        t_.ms[p_] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_).count();
        t_.ran[p_] = true;
    }
    PhaseScope(const PhaseScope &) = delete;
    PhaseScope &operator=(const PhaseScope &) = delete;

private:
    PhaseTimes &t_;
    PhaseTimes::Phase p_;
    gpu::Device *dev_;
    std::chrono::steady_clock::time_point t0_;
};

}  // namespace ntscrt
