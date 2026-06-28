#pragma once
#include <cstddef>
#include <cstdint>

namespace redscope {
class PreallocatedBuffer;
struct Snapshot;
}

namespace redscope::report {

struct CrashFingerprint {
    uint64_t primary = 0;
    uint64_t loose   = 0;
    uint64_t modSet  = 0;
    char     primaryId[12] = {};
    char     looseId[12]   = {};
    bool     hasModule = false;
    bool     faultSiteUnreliable = false;
};

constexpr size_t kMaxFingerprintTail = 5;

CrashFingerprint ComputeFingerprintCore(uint32_t exceptionCode,
                                        const char* buildId,
                                        const char* moduleNameLower,
                                        uint64_t moduleRva,
                                        const char* symbolName,
                                        const char* const* tailNames,
                                        size_t tailCount,
                                        uint64_t modSetHash,
                                        bool faultSiteUnreliable) noexcept;

uint64_t ComputeModSetHash(const Snapshot* snapshot) noexcept;

CrashFingerprint ComputeFingerprint(uint32_t exceptionCode,
                                    uintptr_t faultingRip,
                                    const Snapshot* snapshot,
                                    const char* symbolName) noexcept;

void EmitCrashId(PreallocatedBuffer& out, const CrashFingerprint& fp) noexcept;

void SetLastFingerprint(const CrashFingerprint& fp) noexcept;
const CrashFingerprint& GetLastFingerprint() noexcept;

}
