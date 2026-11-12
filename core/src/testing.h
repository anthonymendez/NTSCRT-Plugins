// testing.h — introspection and fault injection for unit tests (not part of
// the C ABI; tests link ntscrt_core statically).
#pragma once

#include <ntscrt/ntscrt.h>

#include <cstddef>

namespace ntscrt::testing {

struct Stats {
    size_t source_frames = 0;  // frames in the source cache
    size_t source_bytes = 0;   // their pixels
    size_t source_budget = 0;  // the cache's byte budget
    size_t look_parses = 0;    // look JSON strings parsed (identical strings are parsed once)
    int device_resets = 0;     // GPU devices recreated after a loss (since the last good frame)
    bool has_gpu = false;
    void *gpu_device = nullptr;  // native device (ID3D11Device* / id<MTLDevice>): shared ones compare equal
};
Stats stats(ntscrt_instance *inst);

// Source cache byte budget (default 512 MB, or $NTSCRT_SOURCE_CACHE_MB).
void set_source_budget(ntscrt_instance *inst, size_t bytes);

// The next GPU render fails as if the device had been removed. With
// `recreate_fails`, making a new device fails too.
void simulate_device_lost(ntscrt_instance *inst, bool recreate_fails = false);

// Where the last render call's time went, in ms of the calling thread's wall
// time (benchmark: tests/core/bench_transfer.cpp). GPU work runs
// asynchronously: gpu_stages is the time to submit it, the wait for it shows
// up in readback. other = total - the rest (crop, look parsing, ...).
struct PhaseTimes {
    double convert_in = 0;   // host picture -> internal BGRA8 (to_bgra8)
    double cpu_stages = 0;   // Digital video, ntsc-rs
    double upload = 0;       // CPU -> GPU texture uploads
    double gpu_stages = 0;   // Downscale, Glitch, CRT, Screen Loop (submission)
    double readback = 0;     // GPU -> CPU, including the wait for the GPU
    double convert_out = 0;  // result -> host picture (alpha merge, format)
    double total = 0;        // the whole ntscrt_render_cpu call
    double other() const {
        return total - convert_in - cpu_stages - upload - gpu_stages - readback - convert_out;
    }
};
PhaseTimes last_phases(ntscrt_instance *inst);

}  // namespace ntscrt::testing
