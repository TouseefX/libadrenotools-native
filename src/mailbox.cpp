// SPDX-License-Identifier: BSD-2-Clause
// Copyright © 2025 AdrenoTools contributors

#include <adrenotools/mailbox.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include <android/log.h>

#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, "AdrenoToolsMailbox", __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, "AdrenoToolsMailbox", __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AdrenoToolsMailbox", __VA_ARGS__)

int adrenotools_mailbox_open(const char *dev) {
    if (!dev) {
        ALOGE("adrenotools_mailbox_open: null device path");
        return -1;
    }

    // O_RDWR is needed to both read status and ring the doorbell.
    // O_CLOEXEC so the fd is not inherited across exec/spawn.
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ALOGW("adrenotools_mailbox_open: cannot open %s (errno %d)", dev, errno);
        return -1;
    }
    ALOGI("adrenotools_mailbox_open: opened %s (fd %d)", dev, fd);
    return fd;
}

bool adrenotools_mailbox_read(int fd, uint32_t offset, uint32_t *val) {
    if (fd < 0 || !val) {
        ALOGE("adrenotools_mailbox_read: invalid arguments");
        return false;
    }

    uint32_t out = 0;
    ssize_t n = pread(fd, &out, sizeof(out), static_cast<off_t>(offset));
    if (n != static_cast<ssize_t>(sizeof(out))) {
        ALOGW("adrenotools_mailbox_read: pread offset %u returned %zd (errno %d)",
              offset, n, errno);
        return false;
    }

    *val = out;
    ALOGI("adrenotools_mailbox_read: offset 0x%x -> 0x%x", offset, out);
    return true;
}

bool adrenotools_mailbox_write(int fd, uint32_t offset, uint32_t val) {
    if (fd < 0) {
        ALOGE("adrenotools_mailbox_write: invalid fd");
        return false;
    }

    ssize_t n = pwrite(fd, &val, sizeof(val), static_cast<off_t>(offset));
    if (n != static_cast<ssize_t>(sizeof(val))) {
        ALOGW("adrenotools_mailbox_write: pwrite offset 0x%x returned %zd (errno %d)",
              offset, n, errno);
        return false;
    }

    ALOGI("adrenotools_mailbox_write: offset 0x%x <- 0x%x", offset, val);
    return true;
}

void adrenotools_mailbox_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}
