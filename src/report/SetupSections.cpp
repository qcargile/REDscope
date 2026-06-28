#include "Sections.h"
#include "RsdbResolver.h"
#include "ResourceAtFault.h"
#include "../snapshot/Snapshot.h"
#include "../snapshot/SnapshotWorker.h"

namespace redscope::report {

void EmitSetupIntegritySection(PreallocatedBuffer& out) {
    out.Append("--- Setup integrity -----------------------------------------------------------\n");
    const Snapshot* s = redscope::snap::Current();
    if (!s) { out.Append("(snapshot not yet available)\n\n"); return; }

    const auto& si = s->setupIntegrity;
    if (si.issues.empty()) {
        out.Append("(no install problems detected)\n\n");
        return;
    }

    out.Append("Measured facts about your install (independent of this crash):\n");
    for (const auto& iss : si.issues) {
        out.Append("  [");
        out.Append(iss.kind);
        out.Append("] ");
        out.Append(iss.detail);
        out.Append("\n");
    }
    if (si.truncated > 0) {
        out.Appendf("  ... %u more not shown\n", si.truncated);
    }
    out.Append("\n");
}

void EmitArchiveConflictsSection(PreallocatedBuffer& out) {
    out.Append("--- Archive conflicts (curated) -----------------------------------------------\n");
    const Snapshot* s = redscope::snap::Current();
    if (!s) { out.Append("(snapshot not yet available)\n\n"); return; }

    const auto& ac = s->archiveConflicts;
    if (ac.active.empty() && ac.missingDeps.empty()) {
        if (ac.dbModCount == 0) {
            out.Append("(no curated conflict database loaded)\n\n");
        } else {
            out.Append("(no active conflicts among your installed mods)\n\n");
        }
        return;
    }

    out.Append("Leads from the curated conflict database - verify before changing load order.\n");
    if (!ac.active.empty()) {
        out.Append("Conflicting groups (keep one of each):\n");
        for (const auto& g : ac.active) {
            out.Append("  - ");
            for (size_t i = 0; i < g.mods.size(); ++i) {
                if (i) out.Append(", ");
                out.Append(g.mods[i]);
            }
            out.Append("\n");
        }
    }
    if (!ac.missingDeps.empty()) {
        out.Append("Missing dependencies:\n");
        for (const auto& d : ac.missingDeps) {
            out.Append("  - ");
            out.Append(d.mod);
            out.Append(" requires ");
            for (size_t i = 0; i < d.required.size(); ++i) {
                if (i) out.Append(", ");
                out.Append(d.required[i]);
            }
            out.Append("\n");
        }
    }
    out.Append("\n");
}

void EmitResourceLoaderSection(PreallocatedBuffer& out) {
    out.Append("--- Resource streaming at crash -----------------------------------------------\n");
    const Snapshot* s = redscope::snap::Current();
    if (!s) { out.Append("(snapshot not yet available)\n\n"); return; }

    const auto& rl = s->resourceLoader;
    if (!rl.readOk) {
        out.Append("(resource loader state not captured)\n\n");
        return;
    }
    if (rl.failedTotal == 0 && rl.inFlightTotal == 0) {
        out.Append("(no failed or in-flight resources at the last snapshot)\n\n");
        return;
    }

    RsdbDict dict;
    bool haveDict = dict.Open(ResolveRsdbPath());

    const auto& raf = GetLastResourceAtFault();
    if (raf.Any()) {
        out.Append("Referenced on the crashing thread (cross-referenced against the failed/in-flight set):\n");
        for (uint32_t i = 0; i < raf.count; ++i) {
            const auto& e = raf.entries[i];
            const char* origin = e.failed ? "failed" : e.inFlight ? "in-flight" : "loader";
            const char* where  = e.reg[0] ? e.reg : "stack";
            std::string path;
            if (haveDict && dict.Lookup(e.pathHash, path)) {
                out.Appendf("  %s  [%s, via %s]\n", path.c_str(), origin, where);
            } else {
                out.Appendf("  0x%016llX  (%s) [%s, via %s]\n",
                            static_cast<unsigned long long>(e.pathHash),
                            e.failed ? "missing or modded path" : "modded or dynamic path",
                            origin, where);
            }
        }
        out.Append("  (a lead toward the faulting asset, not proof; the faulting reference itself\n"
                   "   may be a freed/sentinel token and absent from this list.)\n");
    }

    auto emitEntry = [&](uint64_t hash, bool failed) {
        std::string path;
        if (haveDict && dict.Lookup(hash, path)) {
            out.Appendf("  %s\n", path.c_str());
        } else {
            out.Appendf("  0x%016llX  (%s)\n",
                        static_cast<unsigned long long>(hash),
                        failed ? "missing or modded path" : "modded or dynamic path");
        }
    };

    uint32_t inFlightShown = 0;
    for (uint32_t i = 0; i < rl.entryCount; ++i) {
        if (!rl.entries[i].inFlight) continue;
        if (inFlightShown == 0) out.Appendf("In flight (%u):\n", rl.inFlightTotal);
        emitEntry(rl.entries[i].pathHash, false);
        ++inFlightShown;
    }
    if (rl.inFlightTotal > inFlightShown && inFlightShown > 0) {
        out.Append("  ... more not shown\n");
    }

    uint32_t failedShown = 0;
    for (uint32_t i = 0; i < rl.entryCount; ++i) {
        if (!rl.entries[i].failed) continue;
        if (failedShown == 0) out.Appendf("Failed to load (%u this session):\n", rl.failedTotal);
        emitEntry(rl.entries[i].pathHash, true);
        ++failedShown;
    }
    if (rl.failedTotal > 0 && failedShown == 0) {
        out.Appendf("Failed to load: %u this session (hashes unavailable)\n", rl.failedTotal);
    } else if (rl.failedTotal > failedShown) {
        out.Appendf("  ... %u more failed this session (snapshot captures up to %u)\n",
                    rl.failedTotal - failedShown,
                    redscope::snap::ResourceLoaderSnapshot::kMaxEntries);
    }

    if (!haveDict) {
        out.Append("(path dictionary not loaded; showing raw hashes)\n");
    }
    out.Append("\n");
}

}
