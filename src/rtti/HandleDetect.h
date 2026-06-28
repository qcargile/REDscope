#pragma once
#include <cstdint>

namespace redscope::rtti {

bool LooksLikeRefBlock(uint32_t strongRefs, uint32_t weakRefs) noexcept;

constexpr uint32_t kMaxPlausibleRefs = 0x100000;

}
