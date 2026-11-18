//! GPU mailbox / doorbell register access (ARM Mali / mail-g52).
//! Memory-safe wrapper around libc::open/pread/pwrite/close.
//! Ported from src/mailbox.cpp to Rust — no raw fd leaks, no data races.

use crate::constants::*;
use libc::{off_t, size_t, ssize_t};

/// Open the GPU mailbox device (e.g. "/dev/mali0").
pub(crate) fn mailbox_open_dev(dev: &str) -> i32 {
    let c_dev = match std::ffi::CString::new(dev) {
        Ok(s) => s,
        Err(_) => return -1,
    };

    // O_RDWR needed to read status and ring doorbell.
    // O_CLOEXEC so fd is not inherited across exec/spawn.
    let fd = unsafe { libc::open(c_dev.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC) };
    if fd < 0 {
        eprintln!("AdrenoToolsMailbox: cannot open {} (open failed)", dev);
        return -1;
    }
    eprintln!("AdrenoToolsMailbox: opened {} (fd {})", dev, fd);
    fd
}

/// Read a 32-bit mailbox register at `offset` bytes from device base.
pub(crate) fn mailbox_read_reg(fd: i32, offset: u32, val: &mut u32) -> bool {
    if fd < 0 {
        eprintln!("AdrenoToolsMailbox: read: invalid fd");
        return false;
    }

    let mut out: u32 = 0;
    let n: ssize_t = unsafe {
        libc::pread(
            fd,
            &mut out as *mut u32 as *mut libc::c_void,
            size_t::try_from(std::mem::size_of::<u32>()).unwrap_or(0),
            offset as off_t,
        )
    };

    if n != std::mem::size_of::<u32>() as ssize_t {
        eprintln!(
            "AdrenoToolsMailbox: read offset 0x{:x} returned {} (pread failed)",
            offset, n
        );
        return false;
    }

    *val = out;
    eprintln!(
        "AdrenoToolsMailbox: read offset 0x{:x} -> 0x{:x}",
        offset, out
    );
    true
}

/// Write a 32-bit value to a mailbox register at `offset` bytes from device base.
pub(crate) fn mailbox_write_reg(fd: i32, offset: u32, val: u32) -> bool {
    if fd < 0 {
        eprintln!("AdrenoToolsMailbox: write: invalid fd");
        return false;
    }

    let n: ssize_t = unsafe {
        libc::pwrite(
            fd,
            &val as *const u32 as *const libc::c_void,
            size_t::try_from(std::mem::size_of::<u32>()).unwrap_or(0),
            offset as off_t,
        )
    };

    if n != std::mem::size_of::<u32>() as ssize_t {
        eprintln!(
            "AdrenoToolsMailbox: write offset 0x{:x} returned {} (pwrite failed)",
            offset, n
        );
        return false;
    }

    eprintln!(
        "AdrenoToolsMailbox: write offset 0x{:x} <- 0x{:x}",
        offset, val
    );
    true
}

/// Close a mailbox descriptor.
pub(crate) fn mailbox_close_fd(fd: i32) {
    if fd >= 0 {
        unsafe { libc::close(fd) };
    }
}

/// Ring the Mali G52 doorbell.
pub(crate) fn mailbox_ring_doorbell_fd(fd: i32) -> bool {
    mailbox_write_reg(fd, MALI_G52_REG_DOORBELL, MALI_G52_DOORBELL_RING)
}
