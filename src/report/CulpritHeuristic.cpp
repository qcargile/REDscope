#include "CulpritHeuristic.h"
#include "../snapshot/Snapshot.h"
#include "../rtti/ObjectsInFlight.h"
#include "../util/StringUtils.h"

#include <cstdio>
#include <cstring>

namespace redscope::report {

namespace {

const snap::PluginInfo* LookupPluginMeta(const Snapshot& s, const char* moduleName) noexcept {
    char key[kModuleNameCap];
    size_t n = LowerToAscii(moduleName, key, sizeof(key));
    auto it = s.pluginMeta.byDll.find(std::string_view(key, n));
    return it == s.pluginMeta.byDll.end() ? nullptr : &it->second;
}

bool IsHostFrameworkModule(const char* name) noexcept {
    return IEqualsAscii(name, "cyber_engine_tweaks.asi")
        || IEqualsAscii(name, "RED4ext.dll")
        || IEqualsAscii(name, "scc.dll");
}

void FormatInto(char* dst, size_t cap, const char* fmt, ...) noexcept {
    if (cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    if (n < 0) dst[0] = '\0';
}

void FormatInFlight(const rtti::InFlightSet* inFlight, char* out, size_t cap) noexcept {
    if (cap == 0) return;
    out[0] = '\0';
    if (!inFlight || inFlight->count == 0) return;

    size_t used = 0;
    auto append = [&](const char* s) {
        size_t n = std::strlen(s);
        if (used + n + 1 >= cap) return;
        std::memcpy(out + used, s, n);
        used += n;
        out[used] = '\0';
    };

    append(" | objects in registers: ");
    uint32_t shown = 0;
    for (uint32_t i = 0; i < inFlight->count && shown < 3; ++i, ++shown) {
        if (shown > 0) append(", ");
        append(inFlight->items[i].className);
    }
    if (inFlight->count > shown) {
        char more[24];
        std::snprintf(more, sizeof(more), " (+%u more)", inFlight->count - shown);
        append(more);
    }
}

}

const char* CategoryLabel(CulpritCategory c) noexcept {
    switch (c) {
        case CulpritCategory::Mod:           return "mod";
        case CulpritCategory::Game:          return "game";
        case CulpritCategory::System:        return "system";
        case CulpritCategory::Memory:        return "memory";
        case CulpritCategory::StackOverflow: return "stack-overflow";
        case CulpritCategory::REDscope:      return "redscope-itself";
        case CulpritCategory::Asset:         return "asset/resource";
        case CulpritCategory::Unknown:
        default:                             return "unknown";
    }
}

const char* ConfidenceLabel(CulpritConfidence c) noexcept {
    switch (c) {
        case CulpritConfidence::High:   return "HIGH";
        case CulpritConfidence::Medium: return "MEDIUM";
        case CulpritConfidence::Low:    return "LOW";
        case CulpritConfidence::Unknown:
        default:                        return "?";
    }
}

CulpritVerdict ComputeVerdict(DWORD exceptionCode,
                              uintptr_t faultingRip,
                              const Snapshot* snapshot,
                              const char* symbolName,
                              const rtti::InFlightSet* inFlight,
                              const char* modOnStackName,
                              uint64_t modOnStackRva) noexcept {
    CulpritVerdict v{};
    char inFlightBuf[160] = {};
    FormatInFlight(inFlight, inFlightBuf, sizeof(inFlightBuf));

    switch (exceptionCode) {
        case EXCEPTION_STACK_OVERFLOW:
            v.category   = CulpritCategory::StackOverflow;
            v.confidence = CulpritConfidence::High;
            FormatInto(v.text, sizeof(v.text),
                       "infinite recursion or excessive call depth - check the native call stack for repeating frames");
            return v;
        case 0xC0000374:
            v.category   = CulpritCategory::Memory;
            v.confidence = CulpritConfidence::High;
            FormatInto(v.text, sizeof(v.text),
                       "heap corruption detected - actual culprit corrupted memory earlier in the session; check the loaded-mod set and recent changes");
            return v;
        case 0xC0000409:
            v.category   = CulpritCategory::Memory;
            v.confidence = CulpritConfidence::High;
            FormatInto(v.text, sizeof(v.text),
                       "stack buffer overrun (/GS check failed) - check frames near RIP for the offending function");
            return v;
        case EXCEPTION_IN_PAGE_ERROR:
            v.category   = CulpritCategory::Memory;
            v.confidence = CulpritConfidence::Medium;
            FormatInto(v.text, sizeof(v.text),
                       "page-in failed - possible bad sector, disconnected drive, or corrupted memory-mapped file");
            return v;
        case 0x887A0005:
        case 0x887A0006:
        case 0x887A0007:
            v.category   = CulpritCategory::System;
            v.confidence = CulpritConfidence::Low;
            FormatInto(v.text, sizeof(v.text),
                       "GPU device removed/reset (TDR 0x%08lX) - a driver/hardware/overclock crash, NOT a mod; update GPU drivers, revert overclocks, check thermals",
                       (unsigned long)exceptionCode);
            return v;
        default:
            break;
    }

    if (!snapshot) {
        v.category   = CulpritCategory::Unknown;
        v.confidence = CulpritConfidence::Unknown;
        FormatInto(v.text, sizeof(v.text),
                   "snapshot unavailable at crash time - RIP 0x%016llX not attributed%s",
                   (unsigned long long)faultingRip, inFlightBuf);
        return v;
    }

    if (snapshot->engineLive.oomSuspected) {
        v.category   = CulpritCategory::Memory;
        v.confidence = CulpritConfidence::Low;
        FormatInto(v.text, sizeof(v.text),
                   "memory/VRAM near-exhausted at crash (%s) - suspected out-of-memory; an environmental crash, not necessarily a specific mod",
                   snapshot->engineLive.oomBasis);
        return v;
    }

    const ModuleInfoFixed* m = FindModuleByPC(*snapshot, faultingRip);
    if (!m) {
        v.category   = CulpritCategory::Unknown;
        v.confidence = CulpritConfidence::Unknown;
        FormatInto(v.text, sizeof(v.text),
                   "RIP 0x%016llX does not lie inside any loaded module%s",
                   (unsigned long long)faultingRip, inFlightBuf);
        return v;
    }

    const char* name = snapshot->moduleNames[m->nameIndex];
    uint64_t    off  = (uint64_t)faultingRip - (uint64_t)m->base;

    if (IEqualsAscii(name, "REDscope.dll")) {
        v.category   = CulpritCategory::REDscope;
        v.confidence = CulpritConfidence::High;
        FormatInto(v.text, sizeof(v.text),
                   "%s+0x%llX - crash inside REDscope itself (likely a dbg_crash self-test; if not, file a bug)",
                   name, (unsigned long long)off);
        return v;
    }

    switch (m->kind) {
        case ModuleKind::Mod: {
            if (IsHostFrameworkModule(name)) {
                v.category   = CulpritCategory::Mod;
                v.confidence = CulpritConfidence::Low;
                FormatInto(v.text, sizeof(v.text),
                           "%s+0x%llX - framework/host module, rarely the real culprit; check LAST ACTIVE and recently-changed mods",
                           name, (unsigned long long)off);
                return v;
            }
            v.category   = CulpritCategory::Mod;
            v.confidence = CulpritConfidence::High;
            const snap::PluginInfo* meta = LookupPluginMeta(*snapshot, name);
            if (meta && !meta->version.empty() && !meta->authors.empty()) {
                FormatInto(v.text, sizeof(v.text),
                           "%s+0x%llX (v%.*s by %.*s)",
                           name, (unsigned long long)off,
                           (int)meta->version.size(), meta->version.data(),
                           (int)meta->authors.size(), meta->authors.data());
            } else if (meta && !meta->version.empty()) {
                FormatInto(v.text, sizeof(v.text),
                           "%s+0x%llX (v%.*s)",
                           name, (unsigned long long)off,
                           (int)meta->version.size(), meta->version.data());
            } else {
                FormatInto(v.text, sizeof(v.text),
                           "%s+0x%llX", name, (unsigned long long)off);
            }
            return v;
        }
        case ModuleKind::Game: {
            if (modOnStackName && modOnStackName[0]) {
                v.category   = CulpritCategory::Mod;
                v.confidence = CulpritConfidence::Medium;
                FormatInto(v.text, sizeof(v.text),
                           "%s+0x%llX on the crashing stack (fault surfaced in %s+0x%llX) - a mod DLL was mid-call when the game faulted; investigate it and recently-changed mods%s",
                           modOnStackName, (unsigned long long)modOnStackRva,
                           name, (unsigned long long)off, inFlightBuf);
                return v;
            }
            v.category   = CulpritCategory::Game;
            v.confidence = CulpritConfidence::Medium;
            if (symbolName && symbolName[0]) {
                FormatInto(v.text, sizeof(v.text),
                           "%s!%s+0x%llX%s",
                           name, symbolName, (unsigned long long)off, inFlightBuf);
            } else {
                FormatInto(v.text, sizeof(v.text),
                           "%s+0x%llX (unsymbolicated)%s",
                           name, (unsigned long long)off, inFlightBuf);
            }
            return v;
        }
        case ModuleKind::System: {
            if (modOnStackName && modOnStackName[0]) {
                v.category   = CulpritCategory::Mod;
                v.confidence = CulpritConfidence::Medium;
                FormatInto(v.text, sizeof(v.text),
                           "%s+0x%llX on the crashing stack (fault surfaced in %s+0x%llX, a system module) - a mod DLL was mid-call; investigate it and recently-changed mods%s",
                           modOnStackName, (unsigned long long)modOnStackRva,
                           name, (unsigned long long)off, inFlightBuf);
                return v;
            }
            v.category   = CulpritCategory::System;
            v.confidence = CulpritConfidence::Low;
            FormatInto(v.text, sizeof(v.text),
                       "%s+0x%llX (system)",
                       name, (unsigned long long)off);
            return v;
        }
        case ModuleKind::Unknown:
        default: {
            v.category   = CulpritCategory::Unknown;
            v.confidence = CulpritConfidence::Low;
            FormatInto(v.text, sizeof(v.text),
                       "%s+0x%llX - unclassified module; could be an injector or overlay%s",
                       name, (unsigned long long)off, inFlightBuf);
            return v;
        }
    }
}

void EmitCulpritLine(PreallocatedBuffer& out, const CulpritVerdict& v) {
    if (v.confidence == CulpritConfidence::Unknown) {
        out.Appendf("LIKELY CULPRIT: %s - %s\n\n",
                    CategoryLabel(v.category), v.text);
    } else {
        out.Appendf("LIKELY CULPRIT: %s (%s) - %s\n\n",
                    CategoryLabel(v.category),
                    ConfidenceLabel(v.confidence),
                    v.text);
    }
}

}
