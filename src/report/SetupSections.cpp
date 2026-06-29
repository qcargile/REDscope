#include "Sections.h"
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

}
