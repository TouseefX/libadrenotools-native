//! Thread-safe global state (replaces racy C++ globals).
//! All access goes through atomic operations — no data races possible.

use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

/// Active Vulkan dispatch function pointers.
/// Replaces the racy `g_active_gipa` / `g_active_gdpa` in C++.
pub struct ActiveDispatch {
    pub gipa: AtomicU64,
    pub gdpa: AtomicU64,
}

impl ActiveDispatch {
    pub fn new() -> Self {
        Self {
            gipa: AtomicU64::new(0),
            gdpa: AtomicU64::new(0),
        }
    }

    pub fn set(&self, gipa: usize, gdpa: usize) {
        self.gipa.store(gipa as u64, Ordering::Release);
        self.gdpa.store(gdpa as u64, Ordering::Release);
    }

    pub fn get_gipa(&self) -> usize {
        self.gipa.load(Ordering::Acquire) as usize
    }

    pub fn get_gdpa(&self) -> usize {
        self.gdpa.load(Ordering::Acquire) as usize
    }
}

/// Whether system/UI callers should be bypassed (replaced `g_hook_system_callers`).
pub static HOOK_SYSTEM_CALLERS: AtomicBool = AtomicBool::new(false);

/// Caller cache epoch for invalidation (replaced `g_caller_cache_epoch`).
pub static CALLER_CACHE_EPOCH: AtomicU64 = AtomicU64::new(1);

/// Global active dispatch — one per process.
static ACTIVE_DISPATCH: OnceLock<ActiveDispatch> = OnceLock::new();

fn get_active_dispatch() -> &'static ActiveDispatch {
    ACTIVE_DISPATCH.get_or_init(ActiveDispatch::new)
}

pub fn state_set_active_dispatch(gipa: usize, gdpa: usize) {
    get_active_dispatch().set(gipa, gdpa);
}

pub fn state_get_active_dispatch_gipa() -> usize {
    get_active_dispatch().get_gipa()
}

pub fn state_get_active_dispatch_gdpa() -> usize {
    get_active_dispatch().get_gdpa()
}

pub fn state_hook_system_callers() -> bool {
    HOOK_SYSTEM_CALLERS.load(Ordering::Relaxed)
}

pub fn state_set_hook_system_callers(enable: bool) {
    HOOK_SYSTEM_CALLERS.store(enable, Ordering::Relaxed);
}

pub fn state_reset_caller_cache() {
    CALLER_CACHE_EPOCH.fetch_add(1, Ordering::Release);
}
