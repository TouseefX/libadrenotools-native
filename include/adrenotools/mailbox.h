// SPDX-License-Identifier: BSD-2-Clause
// Copyright © 2025 AdrenoTools contributors

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GPU mailbox / doorbell register access (ARM Mali / mail-g52).
 *
 * ARM Mali GPUs (e.g. the G52 Bifrost) expose a mailbox/doorbell register that
 * the CPU writes to notify the GPU of newly submitted command buffers. This API
 * opens the GPU device node and lets callers read/write mailbox registers by
 * byte offset.
 *
 * Typical Mali G52 usage:
 *   int fd = adrenotools_mailbox_open("/dev/mali0");
 *   if (fd >= 0) {
 *       adrenotools_mailbox_write(fd, MALI_G52_REG_DOORBELL, MALI_G52_DOORBELL_RING);
 *       adrenotools_mailbox_close(fd);
 *   }
 */

/**
 * @brief Open the GPU mailbox device.
 * @param dev Device path, e.g. "/dev/mali0" for Mali or "/dev/kgsl-3d0" for Adreno.
 * @return A non-negative file descriptor on success, -1 on failure.
 */
int adrenotools_mailbox_open(const char *dev);

/**
 * @brief Read a 32-bit mailbox register at `offset` bytes from the device base.
 * @param fd Descriptor from adrenotools_mailbox_open.
 * @param offset Byte offset of the mailbox register.
 * @param val Out parameter for the read value.
 * @return true on success, false on failure.
 */
bool adrenotools_mailbox_read(int fd, uint32_t offset, uint32_t *val);

/**
 * @brief Write a 32-bit value to a mailbox register at `offset` bytes from the
 *        device base (e.g. ring the doorbell).
 * @param fd Descriptor from adrenotools_mailbox_open.
 * @param offset Byte offset of the mailbox register.
 * @param val Value to write.
 * @return true on success, false on failure.
 */
bool adrenotools_mailbox_write(int fd, uint32_t offset, uint32_t val);

/**
 * @brief Close a mailbox descriptor opened with adrenotools_mailbox_open.
 * @param fd Descriptor to close. -1 is a no-op.
 */
void adrenotools_mailbox_close(int fd);

/** Mali G52 (Bifrost) mailbox / doorbell register offsets (bytes). */
enum mali_g52_mailbox_reg {
    /** Doorbell register: writing a nonzero value rings the GPU doorbell. */
    MALI_G52_REG_DOORBELL      = 0x0000,
    /** GPU status register. */
    MALI_G52_REG_STATUS        = 0x0004,
    /** Interrupt clear register. */
    MALI_G52_REG_IRQ_CLEAR     = 0x0008,
};

/** Value written to the doorbell to ring it. */
#define MALI_G52_DOORBELL_RING 0x1U

#ifdef __cplusplus
}
#endif
