#pragma once
#include <cstddef>

namespace redscope {

inline void CopyFixed(char* dst, size_t cap, const char* src) noexcept {
    if (cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

inline void CopyFixedN(char* dst, size_t cap, const char* src, size_t srcLen) noexcept {
    if (cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < cap && i < srcLen; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

}
