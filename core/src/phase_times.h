// phase_times.h — where one render call's time went. The debug log's phases
// (ntscrt_last_timings) and the benchmark's (testing::last_phases) come from this one clock.
//
// The clock is exclusive: time goes to the current phase, and a PhaseScope
// switches to its own phase and back when it ends, so nested scopes (a readback
// that converts band by band, Screen Loop's uploads and NTSC passes) count once,
// in the innermost phase, and the phases never add up to more than the call's
// wall time (the rest is outside any phase: look parsing, crop, ...).
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
    // in:        host picture -> the pipeline's 8-bit picture (CPU conversion, straight
    //            into upload memory on the direct CPU path; the host GPU path's copy +
    //            convert kernel, and its 8-bit download when a CPU stage needs it)
    // digital, ntsc: the CPU stages     upload: CPU picture -> GPU texture
    // downscale, glitch, crt, loop (Screen Loop's camera): GPU stages (submission
    //            only, unless NTSCRT_PROFILE)
    // readback:  GPU -> CPU, with the wait for the GPU work before it
    // out:       result -> host picture (CPU conversion and alpha merge, or the host
    //            GPU path's merge kernel and copy out)
    enum Phase { In, Digital, Ntsc, Upload, Downscale, Glitch, Crt, Loop, Readback, Out, kCount };
    using Clock = std::chrono::steady_clock;
    const char *path = "cpu";
    double ms[kCount] = {};
    bool ran[kCount] = {};
    double total_ms = 0;  // the whole call (set by finish)
    std::string text;
    std::string note;  // appended to text once (e.g. the host GPU bridge just made)

    void clear(const char *p) {
        path = p;
        note.clear();
        for (int i = 0; i < kCount; ++i) ms[i] = 0, ran[i] = false;
        total_ms = 0;
        cur_ = -1;
        start_ = last_ = Clock::now();
    }
    // The time since the last switch goes to the current phase; from now on to `phase` (-1: none).
    void to(int phase) {
        const auto now = Clock::now();
        if (cur_ >= 0) ms[cur_] += std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
        cur_ = phase;
        if (phase >= 0) ran[phase] = true;
    }
    int current() const { return cur_; }
    void finish() {
        to(-1);
        total_ms = std::chrono::duration<double, std::milli>(last_ - start_).count();
        static const char *names[kCount] = {"in",     "digital", "ntsc", "upload",   "downscale",
                                            "glitch", "crt",     "loop", "readback", "out"};
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

private:
    int cur_ = -1;
    Clock::time_point start_ = Clock::now(), last_ = start_;
};

// Counts a scope as `phase` (scopes nested in it: as theirs); with a device and
// NTSCRT_PROFILE, waits for the GPU work queued in it before it ends.
class PhaseScope {
public:
    PhaseScope(PhaseTimes &t, PhaseTimes::Phase p, gpu::Device *dev = nullptr)
        : t_(t), prev_(t.current()), dev_(dev) {
        t_.to(p);
    }
    ~PhaseScope() {
        if (dev_ && PhaseTimes::profiling()) {
            try {
                dev_->finish();
            } catch (...) {
            }
        }
        t_.to(prev_);
    }
    PhaseScope(const PhaseScope &) = delete;
    PhaseScope &operator=(const PhaseScope &) = delete;

private:
    PhaseTimes &t_;
    int prev_;
    gpu::Device *dev_;
};

}  // namespace ntscrt
