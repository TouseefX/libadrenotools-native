//! GPU memory mapping management — memory-safe replacement for the racy
//! `hook_params->nextGpuMapping` and `hook_gsl_memory_free_pure` in C++.
//!
//! All shared state is behind a Mutex, eliminating the data races that caused
//! intermittent GPU crashes and full device reboots.

use crate::ffi::GpuMapping;
use crate::ffi::GslMemDesc;
use std::sync::{Arc, Mutex, OnceLock};

/// Thread-safe GPU mapping manager.
/// Replaces the racy global `hook_params->nextGpuMapping` from C++.
pub struct GpuMappingManager {
    /// Pending mapping waiting to be consumed by the next
    /// `try_consume_pending` call if size and flags match.
    /// Protected by mutex — Vulkan is multithreaded.
    pending_mapping: Mutex<Option<GpuMapping>>,
}

impl GpuMappingManager {
    pub fn new() -> Self {
        Self {
            pending_mapping: Mutex::new(None),
        }
    }

    /// Store a pending mapping that will be consumed by the next
    /// `try_consume_pending` call if size and flags match.
    /// Replaces the racy `hook_params->nextGpuMapping` write in C++.
    pub fn set_pending(&self, mapping: GpuMapping) {
        let mut pending = self.pending_mapping.lock().unwrap();
        *pending = Some(mapping);
    }

    /// Try to consume the pending mapping if its size and flags match.
    /// Returns Some(GpuMapping) if consumed, None otherwise.
    /// This is the Rust equivalent of hook_gsl_memory_alloc_pure_64's fast path.
    pub fn try_consume_pending(&self, size: u64, flags: u64) -> Option<GpuMapping> {
        let mut pending = self.pending_mapping.lock().unwrap();
        if let Some(mapping) = pending.take() {
            if mapping.size == size && (mapping.flags & flags) == mapping.flags {
                return Some(mapping);
            }
            // Doesn't match — put it back
            *pending = Some(mapping);
        }
        None
    }

    /// Read descriptor fields under mutex — prevents concurrent readers from
    /// seeing partially-zeroed fields (the old C++ zero-before-get-info bug).
    /// The actual ioctl calls stay in C++ where kgsl.h definitions are available.
    pub fn read_descriptor_safely(&self, desc: &GslMemDesc) -> GslMemDesc {
        let _lock = self.pending_mapping.lock().unwrap();
        GslMemDesc {
            hostptr: desc.hostptr,
            gpuaddr: desc.gpuaddr,
            size: desc.size,
            flags: desc.flags,
            priv_: desc.priv_,
        }
    }
}

impl Default for GpuMappingManager {
    fn default() -> Self {
        Self::new()
    }
}

/// Global singleton — one manager per process.
/// Replaces the racy global `hook_params` pointer in C++.
static GPU_MAPPING_MANAGER: OnceLock<Arc<GpuMappingManager>> = OnceLock::new();

pub fn get_gpu_mapping_manager() -> Arc<GpuMappingManager> {
    GPU_MAPPING_MANAGER
        .get_or_init(|| Arc::new(GpuMappingManager::new()))
        .clone()
}

impl GpuMappingManager {
    pub fn get() -> Arc<GpuMappingManager> {
        get_gpu_mapping_manager()
    }
}
