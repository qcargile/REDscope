#pragma once
#include <cstddef>
#include <cstdint>

namespace redscope {

bool SehSafeRead(void* dst, const void* src, size_t bytes) noexcept;

template <typename T>
bool SehSafeReadValue(T& out, const void* src) noexcept {
    return SehSafeRead(&out, src, sizeof(T));
}

bool LooksReadable(const void* addr, size_t bytes) noexcept;

}
