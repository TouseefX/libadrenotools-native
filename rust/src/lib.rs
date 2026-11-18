//! adrenotools-rs — Memory-safe Rust layer for libadrenotools-native.
//!
//! Uses cxx for safe C++/Rust interop (no unsafe extern "C" boundary).
//! Handles:
//!   • GPU memory mapping management (thread-safe, no data races)
//!   • GPU mailbox / doorbell access (Mali G52)
//!   • Thread-safe global state
//!
//! The C++ side keeps ShadowHook, JNI, android linker namespaces, and
//! adrenotools_open_libvulkan — only crash-prone memory management moves
//! to Rust.

mod constants;
mod gpu_mapping;
mod mailbox;
mod state;

// ── cxx bridge ──────────────────────────────────────────────────────────
// This declares the shared types and function signatures that C++ can call.
// cxx generates a C++ header (ffi.h) from this module at build time.
//
// Shared structs are passed by value or reference. Box<T> and Option<T>
// are only supported for opaque types, so we avoid them for shared structs.

#[cxx::bridge(namespace = "ffi")]
mod ffi {
    // ── Shared structs (accessible from both C++ and Rust) ──
    // NOTE: cxx does not allow raw pointers (*mut c_void) in shared structs.
    // Use usize for pointer values; C++ casts them back as needed.
    struct GpuMapping {
        host_ptr: usize,
        gpu_addr: u64,
        size: u64,
        flags: u64,
        magic: u32,
    }

    struct GslMemDesc {
        hostptr: usize,
        gpuaddr: u64,
        size: u64,
        flags: u64,
        priv_: u64,
    }

    // ── Extern "Rust" functions (implemented in Rust, called from C++) ──
    extern "Rust" {
        // GPU mapping management — shared structs passed by value/reference
        fn gpu_mapping_create(host_ptr: usize, gpu_addr: u64, size: u64, flags: u64) -> GpuMapping;
        fn gpu_mapping_destroy(mapping: GpuMapping);
        fn set_pending_mapping(mapping: GpuMapping);
        fn try_consume_pending(size: u64, flags: u64) -> GpuMapping;
        fn read_descriptor_safely(desc: &GslMemDesc) -> GslMemDesc;

        // Mailbox / doorbell (Mali G52)
        fn mailbox_open(dev: &str) -> i32;
        fn mailbox_read(fd: i32, offset: u32, val: &mut u32) -> bool;
        fn mailbox_write(fd: i32, offset: u32, val: u32) -> bool;
        fn mailbox_close(fd: i32);
        fn mailbox_ring_doorbell(fd: i32) -> bool;

        // Thread-safe global state
        fn set_active_dispatch(gipa: usize, gdpa: usize);
        fn get_active_dispatch_gipa() -> usize;
        fn get_active_dispatch_gdpa() -> usize;
        fn hook_system_callers() -> bool;
        fn set_hook_system_callers(enable: bool);
        fn reset_caller_cache();
    }
}

// ── Implementations of the extern "Rust" functions ─────────────────────

use crate::ffi::{GpuMapping, GslMemDesc};
use crate::gpu_mapping::GpuMappingManager;
use crate::mailbox::*;
use crate::state::*;
use crate::constants::ADRENOTOOLS_GPU_MAPPING_SUCCEEDED_MAGIC;

fn gpu_mapping_create(host_ptr: usize, gpu_addr: u64, size: u64, flags: u64) -> GpuMapping {
    GpuMapping {
        host_ptr,
        gpu_addr,
        size,
        flags,
        magic: ADRENOTOOLS_GPU_MAPPING_SUCCEEDED_MAGIC,
    }
}

fn gpu_mapping_destroy(mapping: GpuMapping) {
    drop(mapping);
}

fn set_pending_mapping(mapping: GpuMapping) {
    let manager = GpuMappingManager::get();
    manager.set_pending(mapping);
}

fn try_consume_pending(size: u64, flags: u64) -> GpuMapping {
    let manager = GpuMappingManager::get();
    match manager.try_consume_pending(size, flags) {
        Some(mapping) => mapping,
        None => GpuMapping {
            host_ptr: 0 as usize,
            gpu_addr: 0 as u64,
            size: 0 as u64,
            flags: 0 as u64,
            magic: 0 as u32,
        },
    }
}

fn read_descriptor_safely(desc: &GslMemDesc) -> GslMemDesc {
    let manager = GpuMappingManager::get();
    manager.read_descriptor_safely(desc)
}

// ── Mailbox ────────────────────────────────────────────────────────────

fn mailbox_open(dev: &str) -> i32 {
    mailbox_open_dev(dev)
}

fn mailbox_read(fd: i32, offset: u32, val: &mut u32) -> bool {
    mailbox_read_reg(fd, offset, val)
}

fn mailbox_write(fd: i32, offset: u32, val: u32) -> bool {
    mailbox_write_reg(fd, offset, val)
}

fn mailbox_close(fd: i32) {
    mailbox_close_fd(fd);
}

fn mailbox_ring_doorbell(fd: i32) -> bool {
    mailbox_ring_doorbell_fd(fd)
}

// ── Thread-safe global state ───────────────────────────────────────────

fn set_active_dispatch(gipa: usize, gdpa: usize) {
    state_set_active_dispatch(gipa, gdpa);
}

fn get_active_dispatch_gipa() -> usize {
    state_get_active_dispatch_gipa()
}

fn get_active_dispatch_gdpa() -> usize {
    state_get_active_dispatch_gdpa()
}

fn hook_system_callers() -> bool {
    state_hook_system_callers()
}

fn set_hook_system_callers(enable: bool) {
    state_set_hook_system_callers(enable);
}

fn reset_caller_cache() {
    state_reset_caller_cache();
}
