#include "JsonSidecar.h"
#include "Sections.h"
#include "Fingerprint.h"
#include "CrashClass.h"
#include "ResourceAtFault.h"
#include "../snapshot/Snapshot.h"
#include "../snapshot/InstalledMods.h"
#include "../util/PreallocatedBuffer.h"
#include "../util/SehGuardedRead.h"
#include "../hooks/ScriptStack.h"
#include "../rtti/ObjectsInFlight.h"
#include "../snapshot/WrapChain.h"
#include "../breadcrumbs/BreadcrumbStore.h"

#include <windows.h>
#include <chrono>
#include <cstring>

namespace redscope::report {

namespace {

constexpr uint32_t kMaxScriptedFrames = 32;
constexpr uint32_t kStackModuleScanSlots = 512;
constexpr size_t   kMaxStackModules = 48;
constexpr int      kSidecarSchema = 12;

struct StackModuleHit {
    uint16_t   nameIndex = 0;
    ModuleKind kind      = ModuleKind::Unknown;
    uint32_t   hits      = 0;
};

struct StackModuleScan {
    StackModuleHit hits[kMaxStackModules];
    size_t   count        = 0;
    uint32_t slotsScanned = 0;
    bool     overflow     = false;
};

const char* ModuleKindLabel(ModuleKind k) noexcept {
    switch (k) {
        case ModuleKind::Mod:    return "mod";
        case ModuleKind::Game:   return "game";
        case ModuleKind::System: return "system";
        case ModuleKind::Unknown:
        default:                 return "unknown";
    }
}

void CollectStackModules(const EXCEPTION_POINTERS* ep,
                         const Snapshot& snap,
                         StackModuleScan& r) noexcept {
    if (!ep || !ep->ContextRecord) return;
    const uintptr_t rsp = static_cast<uintptr_t>(ep->ContextRecord->Rsp);
    if (rsp == 0) return;

    uint64_t staging[kStackModuleScanSlots];
    uint32_t readable = 0;
    if (redscope::SehSafeRead(staging, reinterpret_cast<const void*>(rsp), sizeof(staging))) {
        readable = kStackModuleScanSlots;
    } else {
        for (; readable < kStackModuleScanSlots; ++readable) {
            const void* slot = reinterpret_cast<const void*>(rsp + static_cast<uintptr_t>(readable) * 8u);
            if (!redscope::SehSafeReadValue<uint64_t>(staging[readable], slot)) break;
        }
    }
    r.slotsScanned = readable;

    for (uint32_t i = 0; i < readable; ++i) {
        const uint64_t qword = staging[i];
        if (qword == 0) continue;
        const ModuleInfoFixed* m = FindModuleByPC(snap, static_cast<uintptr_t>(qword));
        if (!m || m->nameIndex >= kMaxModules) continue;
        bool found = false;
        for (size_t k = 0; k < r.count; ++k) {
            if (r.hits[k].nameIndex == m->nameIndex) { ++r.hits[k].hits; found = true; break; }
        }
        if (!found) {
            if (r.count >= kMaxStackModules) { r.overflow = true; continue; }
            r.hits[r.count].nameIndex = m->nameIndex;
            r.hits[r.count].kind      = m->kind;
            r.hits[r.count].hits      = 1;
            ++r.count;
        }
    }
}

void AppendJsonStringN(PreallocatedBuffer& out, const char* s, size_t n) noexcept {
    out.AppendChar('"');
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out.Append("\\\""); break;
            case '\\': out.Append("\\\\"); break;
            case '\n': out.Append("\\n");  break;
            case '\r': out.Append("\\r");  break;
            case '\t': out.Append("\\t");  break;
            default:
                if (c < 0x20) {
                    out.Appendf("\\u%04X", static_cast<unsigned>(c));
                } else {
                    out.AppendChar(static_cast<char>(c));
                }
        }
    }
    out.AppendChar('"');
}

void AppendJsonString(PreallocatedBuffer& out, const char* s) noexcept {
    AppendJsonStringN(out, s ? s : "", s ? std::strlen(s) : 0);
}

size_t BoundedLen(const char* s, size_t cap) noexcept {
    if (!s) return 0;
    size_t n = 0;
    while (n < cap && s[n] != '\0') ++n;
    return n;
}

void EmitModInventory(PreallocatedBuffer& out, const Snapshot* snapshot) {
    {
        const char* mgrName = "none";
        if (snapshot) {
            switch (snapshot->inventory.manager) {
                case snap::ModManager::MO2:    mgrName = "MO2"; break;
                case snap::ModManager::Vortex: mgrName = "Vortex"; break;
                case snap::ModManager::None:
                default:                       mgrName = "none"; break;
            }
        }
        out.Append("  \"modManager\": ");
        AppendJsonString(out, mgrName);
        out.Append(",\n");

        out.Append("  \"modManagerDetails\": ");
        if (snapshot) {
            AppendJsonStringN(out, snapshot->inventory.managerDetails.data(),
                              snapshot->inventory.managerDetails.size());
        } else {
            AppendJsonString(out, "");
        }
        out.Append(",\n");

        out.Appendf("  \"modTruncated\": %u,\n",
                    snapshot ? snapshot->inventory.truncated : 0u);
    }

    auto modTypeName = [](snap::ModType t) -> const char* {
        switch (t) {
            case snap::ModType::Red4ext:   return "red4ext";
            case snap::ModType::Cet:       return "cet";
            case snap::ModType::RedScript: return "redscript";
            case snap::ModType::TweakXL:   return "tweakxl";
            case snap::ModType::Archive:   return "archive";
            case snap::ModType::Asi:       return "asi";
            case snap::ModType::Unknown:
            default:                       return "unknown";
        }
    };

    out.Append("  \"installedMods\": [");
    if (snapshot) {
        const auto& mods = snapshot->inventory.mods;
        size_t count = mods.size();
        if (count > snap::kMaxInstalledMods) count = snap::kMaxInstalledMods;
        uint32_t emitted = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto& m = mods[i];
            if (m.name.empty()) continue;
            if (emitted) out.Append(", ");
            out.Append("{ \"name\": ");
            AppendJsonStringN(out, m.name.data(), m.name.size());
            out.Append(", \"version\": ");
            AppendJsonStringN(out, m.version.data(), m.version.size());
            out.Appendf(", \"enabled\": %s, \"type\": ", m.enabled ? "true" : "false");
            AppendJsonString(out, modTypeName(m.type));
            if (!m.subpath.empty()) {
                out.Append(", \"subpath\": ");
                AppendJsonStringN(out, m.subpath.data(), m.subpath.size());
            }
            if (!m.archives.empty()) {
                out.Append(", \"archives\": [");
                for (size_t a = 0; a < m.archives.size(); ++a) {
                    if (a) out.Append(", ");
                    AppendJsonStringN(out, m.archives[a].data(), m.archives[a].size());
                }
                out.Append("]");
            }
            out.Append(" }");
            ++emitted;
        }
    }
    out.Append("]\n");
}

void EmitHardwareJson(PreallocatedBuffer& out, const Snapshot* snapshot) {
    out.Append("  \"hardware\": ");
    if (!snapshot) { out.Append("null,\n"); return; }
    const auto& hw = snapshot->hardware;
    out.Append("{ \"cpu\": ");
    AppendJsonStringN(out, hw.cpuName.data(), hw.cpuName.size());
    out.Appendf(", \"cpuCores\": %u, \"cpuThreads\": %u, \"cpuMaxMHz\": %u",
                hw.cpuPhysicalCores, hw.cpuLogicalCores, hw.cpuMaxTurboMHz);
    if (!hw.gpus.empty()) {
        out.Append(", \"gpu\": ");
        AppendJsonStringN(out, hw.gpus[0].name.data(), hw.gpus[0].name.size());
        out.Append(", \"gpuDriver\": ");
        AppendJsonStringN(out, hw.gpus[0].driverVersion.data(), hw.gpus[0].driverVersion.size());
        out.Appendf(", \"vramMB\": %llu",
                    (unsigned long long)(hw.gpus[0].vramDedicatedBytes / (1024ull * 1024ull)));
    }
    out.Appendf(", \"ramTotalMB\": %llu",
                (unsigned long long)(hw.ramTotalBytes / (1024ull * 1024ull)));
    out.Append(", \"ramType\": ");
    AppendJsonStringN(out, hw.ramType.data(), hw.ramType.size());
    out.Appendf(", \"ramSpeedMHz\": %u", hw.ramSpeedMHz);
    out.Append(", \"os\": ");
    AppendJsonStringN(out, hw.osProductName.data(), hw.osProductName.size());
    out.Append(", \"osVersion\": ");
    AppendJsonStringN(out, hw.osDisplayVersion.data(), hw.osDisplayVersion.size());
    out.Appendf(", \"osBuild\": %u", hw.osBuild);
    out.Appendf(", \"displayW\": %u, \"displayH\": %u, \"displayHz\": %u",
                hw.displayWidth, hw.displayHeight, hw.displayRefreshHz);
    out.Append(", \"displayHdr\": ");
    AppendJsonStringN(out, hw.displayHdrState.data(), hw.displayHdrState.size());
    out.Append(" },\n");
}

void EmitArchiveConflicts(PreallocatedBuffer& out, const Snapshot* snapshot) {
    out.Append("  \"archiveConflicts\": ");
    if (!snapshot) { out.Append("null,\n"); return; }
    const auto& ac = snapshot->archiveConflicts;

    out.Append("{ \"curated\": { \"active\": [");
    for (size_t i = 0; i < ac.active.size(); ++i) {
        if (i) out.Append(", ");
        out.Append("{ \"mods\": [");
        const auto& mods = ac.active[i].mods;
        for (size_t j = 0; j < mods.size(); ++j) {
            if (j) out.Append(", ");
            AppendJsonStringN(out, mods[j].data(), mods[j].size());
        }
        out.Append("] }");
    }
    out.Append("], \"missingDeps\": [");
    for (size_t i = 0; i < ac.missingDeps.size(); ++i) {
        if (i) out.Append(", ");
        out.Append("{ \"mod\": ");
        AppendJsonStringN(out, ac.missingDeps[i].mod.data(), ac.missingDeps[i].mod.size());
        out.Append(", \"requires\": [");
        const auto& req = ac.missingDeps[i].required;
        for (size_t j = 0; j < req.size(); ++j) {
            if (j) out.Append(", ");
            AppendJsonStringN(out, req[j].data(), req[j].size());
        }
        out.Append("] }");
    }
    out.Appendf("], \"dbModCount\": %u, \"truncated\": %u } },\n",
                ac.dbModCount, ac.truncated);
}

void EmitSetupIntegrity(PreallocatedBuffer& out, const Snapshot* snapshot) {
    out.Append("  \"setupIntegrity\": ");
    if (!snapshot) { out.Append("null,\n"); return; }
    const auto& si = snapshot->setupIntegrity;

    out.Append("{ \"issues\": [");
    for (size_t i = 0; i < si.issues.size(); ++i) {
        if (i) out.Append(", ");
        out.Append("{ \"kind\": ");
        AppendJsonStringN(out, si.issues[i].kind.data(), si.issues[i].kind.size());
        out.Append(", \"detail\": ");
        AppendJsonStringN(out, si.issues[i].detail.data(), si.issues[i].detail.size());
        out.Append(" }");
    }
    out.Appendf("], \"truncated\": %u },\n", si.truncated);
}

void EmitResourceLoader(PreallocatedBuffer& out, const Snapshot* snapshot) {
    out.Append("  \"resourceLoader\": ");
    if (!snapshot || !snapshot->resourceLoader.readOk) { out.Append("null,\n"); return; }
    const auto& rl = snapshot->resourceLoader;
    out.Appendf("{ \"readOk\": true, \"failedTotal\": %u, \"inFlightTotal\": %u, \"captured\": %u, \"failed\": [",
                rl.failedTotal, rl.inFlightTotal, rl.entryCount);
    bool first = true;
    for (uint32_t i = 0; i < rl.entryCount; ++i) {
        if (!rl.entries[i].failed) continue;
        if (!first) out.Append(", ");
        first = false;
        out.Appendf("{ \"hash\": \"0x%016llX\", \"error\": %u }",
                    static_cast<unsigned long long>(rl.entries[i].pathHash), rl.entries[i].error);
    }
    out.Append("], \"inFlight\": [");
    first = true;
    for (uint32_t i = 0; i < rl.entryCount; ++i) {
        if (!rl.entries[i].inFlight) continue;
        if (!first) out.Append(", ");
        first = false;
        out.Appendf("\"0x%016llX\"", static_cast<unsigned long long>(rl.entries[i].pathHash));
    }
    out.Append("] },\n");
}

void EmitResourceAtFault(PreallocatedBuffer& out) {
    const auto& raf = GetLastResourceAtFault();
    out.Append("  \"resourceAtFault\": { \"scanned\": ");
    out.Append(raf.scanned ? "true" : "false");
    out.Appendf(", \"stackHits\": %u, \"entries\": [", raf.stackHits);
    for (uint32_t i = 0; i < raf.count; ++i) {
        const auto& e = raf.entries[i];
        if (i) out.Append(", ");
        out.Appendf("{ \"hash\": \"0x%016llX\", \"failed\": %s, \"inFlight\": %s, \"viaPointer\": %s, \"reg\": ",
                    static_cast<unsigned long long>(e.pathHash),
                    e.failed ? "true" : "false",
                    e.inFlight ? "true" : "false",
                    e.viaPointer ? "true" : "false");
        AppendJsonString(out, e.reg);
        out.Append(" }");
    }
    out.Append("] },\n");
}

void EmitBreadcrumbsJson(PreallocatedBuffer& out) {
    out.Append("  \"breadcrumbs\": [");
    auto& store = redscope::GetBreadcrumbStore();
    int64_t crashNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    constexpr size_t kMax = 32;
    redscope::Breadcrumb buf[kMax] = {};
    size_t writeIdx = 0, totalSeen = 0;
    store.ring.Snapshot([&](const redscope::Breadcrumb& b) {
        buf[writeIdx] = b;
        writeIdx = (writeIdx + 1) % kMax;
        ++totalSeen;
    });
    const size_t count = (totalSeen < kMax) ? totalSeen : kMax;
    const size_t startIdx = (totalSeen < kMax) ? 0 : writeIdx;
    for (size_t i = 0; i < count; ++i) {
        const auto& b = buf[(startIdx + i) % kMax];
        if (i) out.Append(", ");
        double secs = static_cast<double>(b.timestampNs - crashNs) / 1e9;
        out.Appendf("{ \"relToCrashSecs\": %.3f, \"kind\": %u, \"tag\": ", secs, b.kind);
        AppendJsonStringN(out, b.tag, BoundedLen(b.tag, sizeof(b.tag)));
        out.Append(", \"message\": ");
        AppendJsonStringN(out, b.message, BoundedLen(b.message, sizeof(b.message)));
        out.Append(" }");
    }
    out.Append("],\n");
}

}

void BuildJsonSidecar(PreallocatedBuffer& out,
                      const EXCEPTION_POINTERS* ep,
                      const Snapshot* snapshot,
                      std::chrono::system_clock::time_point crashTime,
                      const rtti::InFlightSet* inFlight) noexcept {
    const CrashFingerprint& fp = GetLastFingerprint();

    DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    uint64_t addr = (ep && ep->ExceptionRecord)
                        ? reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress)
                        : 0;
    long long unixTime = static_cast<long long>(std::chrono::system_clock::to_time_t(crashTime));

    out.Append("{\n");
    out.Appendf("  \"schema\": %d,\n", kSidecarSchema);
    out.Append("  \"tool\": \"REDscope\",\n");
    out.Appendf("  \"crashTimeUnix\": %lld,\n", unixTime);
    out.Append("  \"crashId\": ");  AppendJsonString(out, fp.primaryId); out.Append(",\n");
    out.Append("  \"looseId\": ");  AppendJsonString(out, fp.looseId);   out.Append(",\n");

    out.Append("  \"fingerprint\": {\n");
    out.Appendf("    \"primary\": \"%016llX\",\n", static_cast<unsigned long long>(fp.primary));
    out.Appendf("    \"loose\": \"%016llX\",\n",   static_cast<unsigned long long>(fp.loose));
    out.Appendf("    \"modSet\": \"%016llX\",\n",  static_cast<unsigned long long>(fp.modSet));
    out.Appendf("    \"faultSiteUnreliable\": %s,\n", fp.faultSiteUnreliable ? "true" : "false");
    out.Appendf("    \"hasModule\": %s\n", fp.hasModule ? "true" : "false");
    out.Append("  },\n");

    out.Append("  \"exception\": {\n");
    out.Appendf("    \"code\": \"0x%08lX\",\n", code);
    out.Appendf("    \"address\": \"0x%016llX\"\n", static_cast<unsigned long long>(addr));
    out.Append("  },\n");

    const char* modName = nullptr;
    uint64_t rva = 0;
    if (snapshot && addr) {
        if (auto* m = FindModuleByPC(*snapshot, static_cast<uintptr_t>(addr))) {
            modName = snapshot->moduleNames[m->nameIndex];
            rva = addr - static_cast<uint64_t>(m->base);
        }
    }
    if (modName) {
        out.Append("  \"faultingModule\": { \"name\": ");
        AppendJsonString(out, modName);
        out.Appendf(", \"rva\": \"0x%llX\" },\n", static_cast<unsigned long long>(rva));
    } else {
        out.Append("  \"faultingModule\": null,\n");
    }

    out.Append("  \"objectsInFlight\": [");
    {
        rtti::InFlightSet decoded;
        const rtti::InFlightSet* set = inFlight;
        if (!set && ep && ep->ContextRecord) {
            decoded = rtti::DecodeObjectsInFlight(*ep->ContextRecord);
            set = &decoded;
        }
        if (set) {
            for (uint32_t i = 0; i < set->count; ++i) {
                if (i) out.Append(", ");
                out.Append("{ \"reg\": ");
                AppendJsonString(out, set->items[i].reg);
                out.Append(", \"className\": ");
                AppendJsonString(out, set->items[i].className);
                out.Appendf(", \"modFields\": %u }", set->items[i].modFields);
            }
        }
    }
    out.Append("],\n");

    out.Append("  \"stackObjects\": [");
    {
        const rtti::InFlightSet& stk = rtti::GetLastStackObjects();
        for (uint32_t i = 0; i < stk.count; ++i) {
            if (i) out.Append(", ");
            out.Append("{ \"className\": ");
            AppendJsonString(out, stk.items[i].className);
            out.Appendf(", \"modFields\": %u, \"sp\": %u }", stk.items[i].modFields, stk.items[i].stackOffset);
        }
    }
    out.Append("],\n");

    {
        StackModuleScan scan{};
        if (snapshot) CollectStackModules(ep, *snapshot, scan);
        for (size_t a = 1; a < scan.count; ++a) {
            StackModuleHit key = scan.hits[a];
            size_t b = a;
            while (b > 0 && scan.hits[b - 1].hits < key.hits) { scan.hits[b] = scan.hits[b - 1]; --b; }
            scan.hits[b] = key;
        }
        out.Append("  \"stackModules\": [");
        for (size_t a = 0; a < scan.count; ++a) {
            if (a) out.Append(", ");
            out.Append("{ \"name\": ");
            AppendJsonString(out, snapshot->moduleNames[scan.hits[a].nameIndex]);
            out.Appendf(", \"hits\": %u, \"kind\": ", scan.hits[a].hits);
            AppendJsonString(out, ModuleKindLabel(scan.hits[a].kind));
            out.Append(" }");
        }
        out.Append("],\n");
        out.Appendf("  \"stackModulesScanned\": %u,\n", scan.slotsScanned);
        out.Appendf("  \"stackModulesOverflow\": %s,\n", scan.overflow ? "true" : "false");
    }

    out.Append("  \"callStackMods\": [");
    if (snapshot && ep && ep->ContextRecord) {
        ModStackFrame frames[48];
        size_t n = GatherModFramesOnStack(ep, snapshot, frames, 48);
        const char* seen[48];
        size_t seenCount = 0;
        uint32_t emitted = 0;
        for (size_t i = 0; i < n; ++i) {
            const char* nm = frames[i].moduleName;
            if (!nm) continue;
            bool dup = false;
            for (size_t k = 0; k < seenCount; ++k) {
                if (seen[k] == nm) { dup = true; break; }
            }
            if (dup) continue;
            seen[seenCount++] = nm;
            if (emitted) out.Append(", ");
            out.Append("{ \"name\": ");
            AppendJsonString(out, nm);
            out.Appendf(", \"topFrame\": %d }", frames[i].frameIndex);
            ++emitted;
        }
    }
    out.Append("],\n");

    out.Append("  \"buildId\": ");
    AppendJsonString(out, snapshot ? snapshot->cp2077BuildString : "");
    out.Append(",\n");
    out.Append("  \"red4extVersion\": ");
    AppendJsonString(out, snapshot ? snapshot->red4extVersion : "");
    out.Append(",\n");

    out.Appendf("  \"gameUptimeNs\": %lld,\n",
                (long long)(snapshot ? snapshot->gameUptimeNs : 0));

    out.Append("  \"crashClass\": ");
    AppendJsonString(out, CrashClassLabel(CurrentCrashClass()));
    out.Append(",\n");

    out.Append("  \"engineState\": ");
    if (snapshot && snapshot->engineLive.hasData) {
        const auto& e = snapshot->engineLive;
        out.Append("{ ");
        if (e.engineReadOk) {
            out.Append("\"state\": ");
            AppendJsonString(out, redscope::snap::EngineStateName(e.engineStateRaw));
            out.Appendf(", \"scriptsLoaded\": %s, \"closing\": %s, \"isEP1\": %s",
                        e.scriptsLoaded ? "true" : "false",
                        e.isClosing ? "true" : "false",
                        e.isEP1 ? "true" : "false");
        } else {
            out.Append("\"engineRead\": false");
        }
        if (e.gpuReadOk) {
            out.Appendf(", \"gpuUsedMB\": %u, \"gpuBudgetMB\": %u, \"gpuTotalMB\": %u",
                        e.gpuUsedMB, e.gpuBudgetMB, e.gpuDedicatedMB);
        }
        out.Appendf(", \"oomSuspected\": %s", e.oomSuspected ? "true" : "false");
        if (e.oomBasis[0]) {
            out.Append(", \"oomBasis\": ");
            AppendJsonString(out, e.oomBasis);
        }
        out.Append(" },\n");
    } else {
        out.Append("null,\n");
    }

    out.Append("  \"scriptedFrames\": [");
    {
        const auto& ss = scriptstack::Current();
        uint32_t emitted = 0;
        for (uint32_t i = 0; i < ss.depth && emitted < kMaxScriptedFrames; ++i) {
            const auto& fr = ss.frames[ss.depth - 1 - i];
            if (!fr.fullName) continue;
            if (emitted) out.Append(", ");
            if (fr.thisClassName && fr.thisClassName[0]) {
                char joined[256];
                int written = std::snprintf(joined, sizeof(joined), "%s on %s",
                                            fr.fullName, fr.thisClassName);
                if (written > 0) AppendJsonStringN(out, joined, std::strlen(joined));
                else AppendJsonString(out, fr.fullName);
            } else {
                AppendJsonString(out, fr.fullName);
            }
            ++emitted;
        }
    }
    out.Append("],\n");

    out.Append("  \"wrapChains\": [");
    if (snapshot && snapshot->wrapChains.scanPerformed) {
        const auto& ss = scriptstack::Current();
        uint32_t emitted = 0;
        const uint32_t kMaxFrames = 8;
        for (uint32_t i = 0; i < ss.depth && emitted < kMaxFrames; ++i) {
            const auto& fr = ss.frames[ss.depth - 1 - i];
            if (!fr.fullName) continue;
            char key[redscope::snap::kWrapMethodKeyCap] = {};
            redscope::snap::NormalizeWrapMethodKey(fr.fullName, key, sizeof(key));
            const auto* chain = redscope::snap::FindChain(snapshot->wrapChains, key);
            if (!chain || chain->layerCount == 0) continue;
            if (emitted) out.Append(", ");
            out.Append("{ \"methodKey\": ");
            AppendJsonString(out, key);
            out.Append(", \"layers\": [");
            for (uint32_t k = 0; k < chain->layerCount; ++k) {
                if (k) out.Append(", ");
                out.Append("{ \"modName\": ");
                AppendJsonStringN(out, chain->layers[k].modName, BoundedLen(chain->layers[k].modName, sizeof(chain->layers[k].modName)));
                out.Append(", \"file\": ");
                AppendJsonStringN(out, chain->layers[k].relFile, BoundedLen(chain->layers[k].relFile, sizeof(chain->layers[k].relFile)));
                out.Appendf(", \"line\": %u }", chain->layers[k].lineNumber);
            }
            out.Append("] }");
            ++emitted;
        }
    }
    out.Append("],\n");

    out.Append("  \"modsChangedSinceLastLaunch\": ");
    if (snapshot && snapshot->modDiff.priorFilePresent && snapshot->modDiff.parsedOk) {
        const auto& d = snapshot->modDiff;
        out.Append("{ \"added\": [");
        size_t na = d.added.size(); if (na > 64) na = 64;
        for (size_t i = 0; i < na; ++i) {
            if (i) out.Append(", ");
            out.Append("{ \"name\": ");
            AppendJsonStringN(out, d.added[i].name.data(), d.added[i].name.size());
            out.Append(", \"version\": ");
            AppendJsonStringN(out, d.added[i].version.data(), d.added[i].version.size());
            out.Append(" }");
        }
        out.Append("], \"removed\": [");
        size_t nr = d.removed.size(); if (nr > 64) nr = 64;
        for (size_t i = 0; i < nr; ++i) {
            if (i) out.Append(", ");
            out.Append("{ \"name\": ");
            AppendJsonStringN(out, d.removed[i].name.data(), d.removed[i].name.size());
            out.Append(" }");
        }
        out.Append("], \"updated\": [");
        size_t nu = d.updated.size(); if (nu > 64) nu = 64;
        for (size_t i = 0; i < nu; ++i) {
            if (i) out.Append(", ");
            out.Append("{ \"name\": ");
            AppendJsonStringN(out, d.updated[i].second.name.data(), d.updated[i].second.name.size());
            out.Append(", \"from\": ");
            AppendJsonStringN(out, d.updated[i].first.version.data(), d.updated[i].first.version.size());
            out.Append(", \"to\": ");
            AppendJsonStringN(out, d.updated[i].second.version.data(), d.updated[i].second.version.size());
            out.Append(" }");
        }
        out.Append("] },\n");
    } else {
        out.Append("null,\n");
    }

    out.Append("  \"gameStateLive\": ");
    if (snapshot && snapshot->gameStateLive.hasData && snapshot->gameStateLive.count > 0) {
        const auto& g = snapshot->gameStateLive;
        out.Append("{ ");
        uint32_t emitted = 0;
        for (uint32_t i = 0; i < g.count; ++i) {
            const auto& e = g.entries[i];
            if (e.key[0] == '\0') continue;
            if (emitted) out.Append(", ");
            AppendJsonString(out, e.key);
            out.Append(": ");
            AppendJsonString(out, e.value);
            ++emitted;
        }
        out.Append(" },\n");
    } else {
        out.Append("null,\n");
    }

    EmitBreadcrumbsJson(out);
    EmitHardwareJson(out, snapshot);
    EmitSetupIntegrity(out, snapshot);
    EmitArchiveConflicts(out, snapshot);
    EmitResourceLoader(out, snapshot);
    EmitResourceAtFault(out);
    EmitModInventory(out, snapshot);
    out.Append("}\n");
}

void BuildDiagnosticJson(const Snapshot* snapshot, PreallocatedBuffer& out) noexcept {
    out.Append("{\n");
    out.Append("  \"reportType\": \"diagnostic\",\n");
    out.Append("  \"tool\": \"REDscope\",\n");
    out.Append("  \"red4extVersion\": ");
    AppendJsonString(out, snapshot ? snapshot->red4extVersion : "");
    out.Append(",\n");
    out.Append("  \"buildId\": ");
    AppendJsonString(out, snapshot ? snapshot->cp2077BuildString : "");
    out.Append(",\n");
    EmitHardwareJson(out, snapshot);
    EmitSetupIntegrity(out, snapshot);
    EmitArchiveConflicts(out, snapshot);
    EmitResourceLoader(out, snapshot);
    EmitModInventory(out, snapshot);
    out.Append("}\n");
}

}
