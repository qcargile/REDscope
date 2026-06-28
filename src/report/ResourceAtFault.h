#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

#include "../snapshot/ResourceLoaderSnapshot.h"

namespace redscope { struct Snapshot; }

namespace redscope::report {

constexpr size_t kMaxResourceAtFault = 8;

struct RegSlot {
    char      name[4] = {};
    uintptr_t value   = 0;
};

struct ResourceAtFaultEntry {
    uint64_t pathHash   = 0;
    bool     failed     = false;
    bool     inFlight   = false;
    bool     viaPointer = false;
    char     reg[4]     = {};
};

struct ResourceAtFaultResult {
    ResourceAtFaultEntry entries[kMaxResourceAtFault];
    uint32_t count     = 0;
    uint32_t stackHits = 0;
    bool     scanned   = false;

    bool Any() const noexcept { return count > 0; }
};

void ScanResourceAtFault(const RegSlot* regs, size_t regCount,
                         const uint64_t* stack, size_t stackCount,
                         const snap::ResourceLoaderSnapshot& known,
                         const snap::ResourceLoaderMemReader& mem,
                         ResourceAtFaultResult& out) noexcept;

ResourceAtFaultResult DecodeResourceAtFault(const CONTEXT& ctx, const Snapshot* snapshot) noexcept;

void SetLastResourceAtFault(const ResourceAtFaultResult& r) noexcept;
const ResourceAtFaultResult& GetLastResourceAtFault() noexcept;

}
