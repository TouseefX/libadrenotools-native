// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cctype>
#include <cstdlib>
#include <strings.h>

// KGSL's gpu_model normally reports e.g. "Adreno (TM) 650". Use the first
// numeric token after Adreno and require exactly three digits to avoid guessing
// from a SoC name, firmware version or partial GPU model number.
inline bool is_adreno_6xx_model(const char *model) {
    if (!model) return false;
    const char *p = model;
    while (*p && strncasecmp(p, "adreno", 6) != 0) ++p;
    if (!*p) return false;
    p += 6;
    while (*p && !std::isdigit(static_cast<unsigned char>(*p))) ++p;
    if (!*p) return false;
    const char *start = p;
    while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
    return p - start == 3 && start[0] == '6';
}

// Conservative workaround for a reported Turnip UBWC NoC fault on a Samsung
// Adreno 650. The cited project applies it to Adreno 6xx; it is not verified on
// every 6xx device. An explicitly supplied TU_DEBUG always wins.
inline bool set_adreno_6xx_turnip_default(const char *model) {
    if (!is_adreno_6xx_model(model) || std::getenv("TU_DEBUG")) return false;
    return setenv("TU_DEBUG", "noubwc", 0) == 0;
}
