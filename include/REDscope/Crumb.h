#pragma once
#include <windows.h>
#include <cstdint>

namespace REDscope {

using CrumbFn = void(*)(uint32_t kind, const char* tag, const char* message);

namespace detail {

inline CrumbFn ResolveCrumb() {
    HMODULE m = ::GetModuleHandleW(L"REDscope.dll");
    if (!m) return nullptr;
    auto p = ::GetProcAddress(m, "REDscope_Crumb");
    return reinterpret_cast<CrumbFn>(p);
}

inline CrumbFn CachedCrumb() {
    static CrumbFn cached = ResolveCrumb();
    return cached;
}

}

inline void Crumb(const char* tag, const char* message) {
    if (auto fn = detail::CachedCrumb()) fn(0, tag, message);
}

}
