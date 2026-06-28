#pragma once
#include <cstdint>
#include <cstddef>

namespace redscope::rtti {

enum class PointerKind {
    Unknown,
    Null,
    Code,
    RttiObject,
    String,
    CName,
    TweakDBID,
    Sentinel,
    OtherData
};

bool MatchSentinelLabel(uint64_t value, char* outLabel, size_t cap) noexcept;

constexpr size_t kPointerInfoNameCap   = 48;
constexpr size_t kPointerInfoSymbolCap = 192;

struct PointerInfo {
    PointerKind kind          = PointerKind::Unknown;
    char        moduleName[kPointerInfoNameCap]     = {};
    uintptr_t   moduleOffset  = 0;
    char        className[kPointerInfoNameCap]      = {};
    char        symbolName[kPointerInfoSymbolCap]   = {};
    uint32_t    symbolOffset  = 0;
};

PointerInfo Inspect(uintptr_t addr) noexcept;

PointerInfo InspectFast(uintptr_t addr) noexcept;

bool TryGetObjectClassName(uintptr_t addr, char* out, size_t cap) noexcept;

}
