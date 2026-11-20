// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>

// KGSL's gpu_model normally reports "Adreno (TM) 650". Do not infer the
// model from the SoC name or a partial number: other GPUs need their defaults.
inline bool is_adreno_650_model(const char *model) {
    if (!model) return false;
    bool has_adreno = false;
    for (const char *p = model; *p; ++p) {
        if (strncasecmp(p, "adreno", 6) == 0) {
            has_adreno = true;
            break;
        }
    }
    if (!has_adreno) return false;
    for (const char *p = model; (p = std::strstr(p, "650")); ++p) {
        const bool left = p == model || !std::isdigit(static_cast<unsigned char>(p[-1]));
        const bool right = !std::isdigit(static_cast<unsigned char>(p[3]));
        if (left && right) return true;
    }
    return false;
}

// Conservative workaround for a reported Turnip UBWC NoC fault on a related
// Adreno 650 Samsung phone. An explicitly supplied TU_DEBUG always wins.
inline bool set_adreno_650_turnip_default(const char *model) {
    if (!is_adreno_650_model(model) || std::getenv("TU_DEBUG")) return false;
    return setenv("TU_DEBUG", "noubwc", 0) == 0;
}
