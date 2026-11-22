//! NTSCRT-Plugins addition to librashader-cache (MPL-2.0 OR GPL-3.0-only, as the rest
//! of the crate), put in by rust/librashader-patch/apply.cmake: runs a host call on a
//! private one-thread rayon pool instead of the caller's thread.
//!
//! The runtimes compile a preset's passes and decode its textures with rayon's
//! `into_par_iter`, on the global pool. A chain built inside `ThreadPool::install`
//! uses that pool instead, so the plug-in's background shader-cache warm-up (which
//! builds chains only to fill the cache, on a low-priority thread) never queues work
//! in the global pool that the chains of real renders compile on.
use std::os::raw::c_void;
use std::sync::OnceLock;

static POOL: OnceLock<Option<rayon::ThreadPool>> = OnceLock::new();

/// Calls `f(user)` on the private pool's one thread and waits for it. Returns 0, or
/// -1 when `f` is null or the pool can't be made (nothing was called then).
/// `f` must not unwind (C++ callers catch everything).
#[no_mangle]
pub unsafe extern "C" fn ntscrt_libra_run_isolated(
    f: Option<unsafe extern "C" fn(*mut c_void)>,
    user: *mut c_void,
) -> i32 {
    let Some(f) = f else { return -1 };
    let pool = POOL.get_or_init(|| {
        rayon::ThreadPoolBuilder::new()
            .num_threads(1)
            .thread_name(|_| "ntscrt-shader-warm".into())
            .build()
            .ok()
    });
    let Some(pool) = pool else { return -2 };
    let user = user as usize; // a raw pointer isn't Send; the caller keeps it alive
    pool.install(move || unsafe { f(user as *mut c_void) });
    0
}
