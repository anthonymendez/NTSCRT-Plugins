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
};
Stats stats(ntscrt_instance *inst);

// Source cache byte budget (default 512 MB, or $NTSCRT_SOURCE_CACHE_MB).
void set_source_budget(ntscrt_instance *inst, size_t bytes);

// The next GPU render fails as if the device had been removed. With
// `recreate_fails`, making a new device fails too.
void simulate_device_lost(ntscrt_instance *inst, bool recreate_fails = false);

}  // namespace ntscrt::testing
