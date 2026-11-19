//! Shared constants used by both Rust and C++ sides.

pub const ADRENOTOOLS_GPU_MAPPING_SUCCEEDED_MAGIC: u32 = 0xDEADBEEF;

// Mali G52 mailbox register offsets (bytes)
pub const MALI_G52_REG_DOORBELL: u32 = 0x0000;

pub const MALI_G52_DOORBELL_RING: u32 = 0x1;
