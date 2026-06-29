#include "Fingerprint.h"
#include "../snapshot/Snapshot.h"
#include "../util/PreallocatedBuffer.h"
#include "../hooks/ScriptStack.h"
#include "../util/StringUtils.h"

#include <windows.h>
#include <cstring>

namespace redscope::report {

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime  = 1099511628211ULL;
constexpr unsigned char kFieldSep = 0x1F;

constexpr uint32_t kHeapCorruption     = 0xC0000374u;
constexpr uint32_t kStackBufferOverrun = 0xC0000409u;

constexpr size_t kIdChars = 8;

uint64_t FnvBytes(uint64_t h, const void* p, size_t n) noexcept {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= kFnvPrime;
    }
    return h;
}

uint64_t FnvStr(uint64_t h, const char* s) noexcept {
    if (!s) return h;
    while (*s) {
        h ^= static_cast<unsigned char>(*s++);
        h *= kFnvPrime;
    }
    return h;
}

uint64_t FnvU64(uint64_t h, uint64_t v) noexcept {
    return FnvBytes(h, &v, sizeof(v));
}

uint64_t FnvSep(uint64_t h) noexcept {
    return FnvBytes(h, &kFieldSep, 1);
}

uint64_t Mix64(uint64_t x) noexcept {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

void EncodeId(uint64_t h, char* out, size_t cap) noexcept {
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    size_t n = kIdChars;
    if (cap == 0) return;
    if (n + 1 > cap) n = cap - 1;
    for (size_t i = 0; i < n; ++i) {
        unsigned idx = static_cast<unsigned>((h >> (59 - 5 * i)) & 0x1F);
        out[i] = alphabet[idx];
    }
    out[n] = '\0';
}

}

CrashFingerprint ComputeFingerprintCore(uint32_t exceptionCode,
                                        const char* buildId,
                                        const char* moduleNameLower,
                                        uint64_t moduleRva,
                                        const char* symbolName,
                                        const char* const* tailNames,
                                        size_t tailCount,
                                        uint64_t modSetHash,
                                        bool faultSiteUnreliable) noexcept {
    CrashFingerprint fp{};
    fp.modSet              = modSetHash;
    fp.hasModule           = (moduleNameLower != nullptr);
    fp.faultSiteUnreliable = faultSiteUnreliable;

    uint64_t base = kFnvOffset;
    base = FnvU64(base, exceptionCode);
    base = FnvSep(base);
    base = FnvStr(base, buildId);
    base = FnvSep(base);
    if (moduleNameLower) {
        base = FnvStr(base, moduleNameLower);
        base = FnvSep(base);
        if (symbolName && symbolName[0]) {
            base = FnvStr(base, symbolName);
        } else {
            base = FnvU64(base, moduleRva);
        }
    }
    fp.loose = base ? base : 1;

    const bool gameUnsymbolicated =
        moduleNameLower &&
        std::strcmp(moduleNameLower, "cyberpunk2077.exe") == 0 &&
        !(symbolName && symbolName[0]);
    if (gameUnsymbolicated && tailNames && tailCount > 0 && tailNames[0]) {
        uint64_t looseEnriched = base;
        looseEnriched = FnvSep(looseEnriched);
        looseEnriched = FnvStr(looseEnriched, tailNames[0]);
        fp.loose = looseEnriched ? looseEnriched : 1;
    }

    uint64_t prim = base;
    if (tailNames && tailCount > 0) {
        size_t n = tailCount > kMaxFingerprintTail ? kMaxFingerprintTail : tailCount;
        for (size_t i = 0; i < n; ++i) {
            prim = FnvSep(prim);
            prim = FnvStr(prim, tailNames[i]);
        }
    }
    fp.primary = prim ? prim : 1;

    EncodeId(fp.primary, fp.primaryId, sizeof(fp.primaryId));
    EncodeId(fp.loose, fp.looseId, sizeof(fp.looseId));
    return fp;
}

uint64_t ComputeModSetHash(const Snapshot* snapshot) noexcept {
    if (!snapshot) return 0;
    const auto& mods = snapshot->inventory.mods;
    size_t count = mods.size();
    if (count > snap::kMaxInstalledMods) count = snap::kMaxInstalledMods;
    uint64_t acc = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& m = mods[i];
        if (!m.enabled) continue;
        if (m.name.empty()) continue;
        uint64_t h = kFnvOffset;
        h = FnvBytes(h, m.name.data(), m.name.size());
        h = FnvSep(h);
        h = FnvBytes(h, m.version.data(), m.version.size());
        acc += Mix64(h);
    }
    return acc;
}

CrashFingerprint ComputeFingerprint(uint32_t exceptionCode,
                                    uintptr_t faultingRip,
                                    const Snapshot* snapshot,
                                    const char* symbolName,
                                    const char* modOnStackName,
                                    uint64_t modOnStackRva) noexcept {
    bool faultSiteUnreliable = exceptionCode == kHeapCorruption
                || exceptionCode == kStackBufferOverrun
                || exceptionCode == static_cast<uint32_t>(EXCEPTION_STACK_OVERFLOW);

    const char* buildId = snapshot ? snapshot->cp2077BuildString : nullptr;

    char nameLower[kModuleNameCap];
    const char* moduleNameLower = nullptr;
    uint64_t rva = 0;
    if (snapshot) {
        const ModuleInfoFixed* m = FindModuleByPC(*snapshot, faultingRip);
        if (m) {
            LowerToAscii(snapshot->moduleNames[m->nameIndex], nameLower, sizeof(nameLower));
            moduleNameLower = nameLower;
            rva = static_cast<uint64_t>(faultingRip) - static_cast<uint64_t>(m->base);
        }
    }

    const char* tail[kMaxFingerprintTail] = {};
    char        tailJoined[kMaxFingerprintTail][192] = {};
    char        modTag[160] = {};
    size_t tailCount = 0;
    auto pushTail = [&](const char* fullName, const char* thisClass) {
        if (!fullName || tailCount >= kMaxFingerprintTail) return;
        if (thisClass && thisClass[0]) {
            std::snprintf(tailJoined[tailCount], sizeof(tailJoined[tailCount]),
                          "%s|%s", fullName, thisClass);
            tail[tailCount] = tailJoined[tailCount];
        } else {
            tail[tailCount] = fullName;
        }
        ++tailCount;
    };
    const auto& ss = scriptstack::Current();
    if (ss.depth > 0) {
        uint32_t d = ss.depth;
        for (uint32_t i = 0; i < kMaxFingerprintTail && i < d; ++i) {
            const auto& fr = ss.frames[d - 1 - i];
            pushTail(fr.fullName, fr.thisClassName);
        }
    }
    if (tailCount == 0 && modOnStackName && modOnStackName[0]) {
        std::snprintf(modTag, sizeof(modTag), "%s+0x%llX",
                      modOnStackName, (unsigned long long)modOnStackRva);
        pushTail(modTag, nullptr);
    }
    if (tailCount == 0) {
        const scriptstack::HeartbeatSlot* slots = nullptr;
        size_t slotCount = 0;
        scriptstack::GetHeartbeatSlots(&slots, &slotCount);
        uint32_t tid = static_cast<uint32_t>(GetCurrentThreadId());
        for (size_t i = 0; i < slotCount; ++i) {
            if (slots[i].threadId == tid && slots[i].lastFullName) {
                pushTail(slots[i].lastFullName, slots[i].lastThisClassName);
                break;
            }
        }
    }

    uint64_t modSet = ComputeModSetHash(snapshot);

    return ComputeFingerprintCore(exceptionCode, buildId, moduleNameLower, rva,
                                  symbolName, tail, tailCount, modSet, faultSiteUnreliable);
}

void EmitCrashId(PreallocatedBuffer& out, const CrashFingerprint& fp) noexcept {
    if (fp.faultSiteUnreliable) {
        out.Appendf("CRASH ID: %s (loose %s) [grouping: fault-site class - distinct bugs may share this bucket]\n",
                    fp.primaryId, fp.looseId);
    } else {
        out.Appendf("CRASH ID: %s (loose %s)\n", fp.primaryId, fp.looseId);
    }
    out.Appendf("  fingerprint %016llX / %016llX  modset %016llX\n\n",
                static_cast<unsigned long long>(fp.primary),
                static_cast<unsigned long long>(fp.loose),
                static_cast<unsigned long long>(fp.modSet));
}

namespace {
CrashFingerprint g_lastFingerprint{};
}

void SetLastFingerprint(const CrashFingerprint& fp) noexcept {
    g_lastFingerprint = fp;
}

const CrashFingerprint& GetLastFingerprint() noexcept {
    return g_lastFingerprint;
}

}
