#pragma once
#include <cstddef>
#include <cstring>

namespace redscope {

inline char ToLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

inline bool IEqualsAscii(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b) {
        if (ToLowerAscii(*a) != ToLowerAscii(*b)) return false;
    }
    return *a == '\0' && *b == '\0';
}

inline bool IContainsAscii(const char* haystack, const char* needle) noexcept {
    if (!haystack || !needle || !*needle) return false;
    const size_t nlen = std::strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        size_t i = 0;
        for (; i < nlen; ++i) {
            if (p[i] == '\0') return false;
            if (ToLowerAscii(p[i]) != ToLowerAscii(needle[i])) break;
        }
        if (i == nlen) return true;
    }
    return false;
}

inline size_t LowerToAscii(const char* src, char* dst, size_t dstCap) noexcept {
    if (dstCap == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }
    size_t i = 0;
    for (; src[i] && i + 1 < dstCap; ++i) dst[i] = ToLowerAscii(src[i]);
    dst[i] = '\0';
    return i;
}

}
