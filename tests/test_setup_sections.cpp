#include <gtest/gtest.h>
#include "../src/report/Sections.h"
#include "../src/snapshot/Snapshot.h"
#include "../src/util/PreallocatedBuffer.h"
#include <string>

namespace redscope::snap { void TestSetCurrentSnapshot(const redscope::Snapshot*) noexcept; }

using redscope::Snapshot;
using redscope::PreallocatedBuffer;
using redscope::report::EmitSetupIntegritySection;
using redscope::report::EmitArchiveConflictsSection;

namespace {

std::string Render(void (*fn)(PreallocatedBuffer&)) {
    PreallocatedBuffer buf;
    buf.Reserve(8192);
    fn(buf);
    std::string out(buf.Data(), buf.Size());
    redscope::snap::TestSetCurrentSnapshot(nullptr);
    return out;
}

}

TEST(SetupIntegritySection, IssuesRendered) {
    Snapshot s{};
    s.setupIntegrity.issues.push_back(
        { "missing-framework", "130 TweakXL mod(s) installed but TweakXL is not loaded." });
    s.setupIntegrity.issues.push_back(
        { "plugin-load-failure", "2 RED4ext plugin(s) failed to load; see the red4ext log." });
    redscope::snap::TestSetCurrentSnapshot(&s);
    auto out = Render(&EmitSetupIntegritySection);
    EXPECT_NE(out.find("Setup integrity"), std::string::npos);
    EXPECT_NE(out.find("[missing-framework]"), std::string::npos);
    EXPECT_NE(out.find("TweakXL is not loaded"), std::string::npos);
    EXPECT_NE(out.find("[plugin-load-failure]"), std::string::npos);
}

TEST(SetupIntegritySection, CleanShowsNoProblems) {
    Snapshot s{};
    redscope::snap::TestSetCurrentSnapshot(&s);
    auto out = Render(&EmitSetupIntegritySection);
    EXPECT_NE(out.find("no install problems"), std::string::npos);
}

TEST(ArchiveConflictsSection, ActiveGroupAndMissingDepRendered) {
    Snapshot s{};
    s.archiveConflicts.dbModCount = 3;
    s.archiveConflicts.active.push_back({ { "ModA", "ModB" } });
    s.archiveConflicts.missingDeps.push_back({ "ModX", { "ModY", "Phantom Liberty" } });
    redscope::snap::TestSetCurrentSnapshot(&s);
    auto out = Render(&EmitArchiveConflictsSection);
    EXPECT_NE(out.find("ModA, ModB"), std::string::npos);
    EXPECT_NE(out.find("ModX requires ModY, Phantom Liberty"), std::string::npos);
}

TEST(ArchiveConflictsSection, EmptyDbShowsNotLoaded) {
    Snapshot s{};
    redscope::snap::TestSetCurrentSnapshot(&s);
    auto out = Render(&EmitArchiveConflictsSection);
    EXPECT_NE(out.find("no curated conflict database"), std::string::npos);
}

TEST(ArchiveConflictsSection, LoadedDbButNoActiveShowsClean) {
    Snapshot s{};
    s.archiveConflicts.dbModCount = 42;
    redscope::snap::TestSetCurrentSnapshot(&s);
    auto out = Render(&EmitArchiveConflictsSection);
    EXPECT_NE(out.find("no active conflicts"), std::string::npos);
}
