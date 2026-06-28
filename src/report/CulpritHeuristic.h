#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

#include "../util/PreallocatedBuffer.h"

namespace redscope {
struct Snapshot;
}

namespace redscope::rtti {
struct InFlightSet;
}

namespace redscope::report {

struct ResourceAtFaultResult;

enum class CulpritConfidence : uint8_t {
    Unknown = 0,
    Low     = 1,
    Medium  = 2,
    High    = 3,
};

enum class CulpritCategory : uint8_t {
    Unknown       = 0,
    Mod           = 1,
    Game          = 2,
    System        = 3,
    Memory        = 4,
    StackOverflow = 5,
    REDscope      = 6,
    Asset         = 7,
};

struct CulpritVerdict {
    CulpritConfidence confidence = CulpritConfidence::Unknown;
    CulpritCategory   category   = CulpritCategory::Unknown;
    char text[512] = {};
};

CulpritVerdict ComputeVerdict(DWORD exceptionCode,
                              uintptr_t faultingRip,
                              const Snapshot* snapshot,
                              const char* symbolName = nullptr,
                              const rtti::InFlightSet* inFlight = nullptr,
                              const ResourceAtFaultResult* resAtFault = nullptr) noexcept;

void EmitCulpritLine(PreallocatedBuffer& out, const CulpritVerdict& v);

const char* CategoryLabel(CulpritCategory c) noexcept;
const char* ConfidenceLabel(CulpritConfidence c) noexcept;

}
