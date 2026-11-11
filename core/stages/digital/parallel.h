// parallel.h — a small portable parallel-for for the Digital video stage
// (replaces upstream's DispatchQueue.concurrentPerform).
//
// Every iteration must write only its own outputs; the split across threads
// then never changes the result, so output is bit-identical for any thread
// count (reductions are done by the callers per iteration, then summed in
// order).
#pragma once

#include <cstddef>
#include <utility>

namespace ntscrt::digital {

// Number of threads parallel_for may use (including the caller).
// 0 = hardware concurrency. Mainly for tests; safe to call while idle.
void set_thread_count(int n);
int thread_count();
// Joins the worker threads (they start again on the next parallel_for).
// Call from ordinary code, never from a static destructor / DllMain; the
// façade calls it when its last instance is destroyed.
void shutdown_threads();
bool threads_running();  // for tests

namespace detail {
using RangeFn = void (*)(void *ctx, int begin, int end);
void parallel_for_impl(int n, RangeFn fn, void *ctx);
}  // namespace detail

// body(i) for i in [0, n).
template <class F>
inline void parallel_for(int n, F &&body) {
    if (n <= 0) return;
    auto *f = &body;
    using FP = decltype(f);
    detail::parallel_for_impl(
        n,
        [](void *ctx, int b, int e) {
            auto &g = *static_cast<FP>(ctx);
            for (int i = b; i < e; ++i) g(i);
        },
        const_cast<void *>(static_cast<const void *>(f)));
}

}  // namespace ntscrt::digital
