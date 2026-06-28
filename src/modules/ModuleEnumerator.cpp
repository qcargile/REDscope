#include "ModuleEnumerator.h"
#include <windows.h>
#include <vector>
#include <cstdio>

namespace redscope {

std::string QueryFileVersionUtf8(const wchar_t* path) {
    DWORD dummy = 0;
    DWORD sz = ::GetFileVersionInfoSizeW(path, &dummy);
    if (!sz) return {};
    std::vector<uint8_t> buf(sz);
    if (!::GetFileVersionInfoW(path, 0, sz, buf.data())) return {};
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiSize = 0;
    if (!::VerQueryValueW(buf.data(), L"\\",
                          reinterpret_cast<LPVOID*>(&ffi), &ffiSize) || !ffi) return {};
    char out[64];
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u",
        HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
        HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return out;
}

}
