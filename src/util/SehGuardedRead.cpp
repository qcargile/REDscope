#include "SehGuardedRead.h"
#include <windows.h>
#include <cstring>

namespace redscope {

bool LooksReadable(const void* addr, size_t bytes) noexcept {
    if (!addr || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    auto base = reinterpret_cast<uintptr_t>(addr);
    if (::VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable)) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return bytes <= regionEnd - base;
}

bool SehSafeRead(void* dst, const void* src, size_t bytes) noexcept {
    if (!LooksReadable(src, bytes)) return false;
    __try {
        std::memcpy(dst, src, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}
