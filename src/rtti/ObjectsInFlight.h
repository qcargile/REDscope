#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

#include "PointerType.h"

namespace redscope::rtti {

struct InFlightObject {
    char     reg[4]                          = {};
    char     className[kPointerInfoNameCap]  = {};
    uint32_t stackOffset                     = 0;
};

constexpr size_t kMaxInFlight = 14;

struct InFlightSet {
    InFlightObject items[kMaxInFlight];
    uint32_t       count = 0;

    bool Any() const noexcept { return count > 0; }
};

InFlightSet DecodeObjectsInFlight(const CONTEXT& ctx) noexcept;

InFlightSet DecodeObjectsOnStack(const CONTEXT& ctx) noexcept;

void SetLastStackObjects(const InFlightSet& set) noexcept;
const InFlightSet& GetLastStackObjects() noexcept;

bool InterpretNullDeref(const CONTEXT& ctx, uintptr_t dataAddr, const char* op,
                        char* out, size_t cap) noexcept;

}
