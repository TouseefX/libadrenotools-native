// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cctype>
#include <strings.h>

// Look for the GPU model after "Adreno" when present, so a preceding SoC
// number (e.g. SM8250) does not select the wrong generation. Some KGSL
// gpu_model files contain just a number, so retain that fallback.
inline int parse_adreno_generation(const char *model) {
    if (!model || !model[0]) return 0;
    const char *start = model;
    for (const char *p = model; *p; ++p) {
        if (strncasecmp(p, "adreno", 6) == 0) {
            start = p + 6;
            break;
        }
    }
    while (*start && !std::isdigit(static_cast<unsigned char>(*start))) ++start;
    if (*start == '6') {
        // Apply 6xx workarounds only to a complete three-digit GPU model,
        // not a partial model, SoC number or longer future GPU identifier.
        const char *end = start;
        while (std::isdigit(static_cast<unsigned char>(*end))) ++end;
        if (end - start != 3) return 0;
    }
    return *start >= '5' && *start <= '9' ? *start - '0' : 0;
}
