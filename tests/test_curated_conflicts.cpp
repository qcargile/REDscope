#include <gtest/gtest.h>
#include "../src/snapshot/CuratedConflicts.h"
#include <string>
#include <vector>

using redscope::snap::ConflictDB;
using redscope::snap::MergeConflictJson;
using redscope::snap::FinalizeConflictDB;
using redscope::snap::EvaluateConflicts;
using redscope::snap::ArchiveConflicts;

namespace {

ConflictDB Build(const std::vector<std::string>& jsons) {
    ConflictDB db;
    for (const auto& j : jsons) MergeConflictJson(db, j);
    FinalizeConflictDB(db);
    return db;
}

bool HasGroup(const ArchiveConflicts& r, const std::vector<std::string>& mods) {
    for (const auto& g : r.active) {
        if (g.mods == mods) return true;
    }
    return false;
}

}

TEST(CuratedConflicts, TagCollisionReportsBothInstalled) {
    auto db = Build({ R"({ "mods": { "A": ["a.archive","env"], "B": ["b.archive","env"] } })" });
    auto r = EvaluateConflicts(db, { "a.archive", "b.archive" }, false);
    EXPECT_TRUE(HasGroup(r, { "A", "B" }));
}

TEST(CuratedConflicts, ExceptionSuppressesTagCollision) {
    auto db = Build({ R"({ "mods": { "A": ["a.archive","env"], "B": ["b.archive","env"] }, "exceptions": [["A","B"]] })" });
    auto r = EvaluateConflicts(db, { "a.archive", "b.archive" }, false);
    EXPECT_TRUE(r.active.empty());
}

TEST(CuratedConflicts, ExplicitConflictBareStringArchives) {
    auto db = Build({ R"({ "mods": { "A": "a.archive", "B": "b.archive" }, "conflicts": [["A","B"]] })" });
    auto r = EvaluateConflicts(db, { "a.archive", "b.archive" }, false);
    EXPECT_TRUE(HasGroup(r, { "A", "B" }));
}

TEST(CuratedConflicts, NotInstalledNoConflict) {
    auto db = Build({ R"({ "mods": { "A": ["a.archive","env"], "B": ["b.archive","env"] } })" });
    auto r = EvaluateConflicts(db, { "a.archive" }, false);
    EXPECT_TRUE(r.active.empty());
}

TEST(CuratedConflicts, CaseInsensitiveArchiveJoin) {
    auto db = Build({ R"({ "mods": { "A": ["a.archive","env"], "B": ["B.ARCHIVE","env"] } })" });
    auto r = EvaluateConflicts(db, { "A.ARCHIVE", "b.archive" }, false);
    EXPECT_TRUE(HasGroup(r, { "A", "B" }));
}

TEST(CuratedConflicts, MissingDependencyAndPhantomLibertySentinel) {
    std::string j = R"({ "mods": { "A": "a.archive", "B": "b.archive" }, "dependencies": { "A": ["B","Phantom Liberty"] } })";
    auto db = Build({ j });

    auto r1 = EvaluateConflicts(db, { "a.archive" }, false);
    ASSERT_EQ(r1.missingDeps.size(), 1u);
    EXPECT_EQ(r1.missingDeps[0].mod, "A");
    EXPECT_EQ(r1.missingDeps[0].required, (std::vector<std::string>{ "B", "Phantom Liberty" }));

    auto r2 = EvaluateConflicts(db, { "a.archive", "b.archive" }, true);
    EXPECT_TRUE(r2.missingDeps.empty());
}

TEST(CuratedConflicts, DependencyNotEvaluatedWhenModNotInstalled) {
    std::string j = R"({ "mods": { "A": "a.archive", "B": "b.archive" }, "dependencies": { "A": ["B"] } })";
    auto db = Build({ j });
    auto r = EvaluateConflicts(db, {}, false);
    EXPECT_TRUE(r.missingDeps.empty());
}

TEST(CuratedConflicts, FirstWinsMergeKeepsFirstDefinition) {
    ConflictDB db;
    MergeConflictJson(db, R"({ "mods": { "A": ["a.archive","env"] } })");
    MergeConflictJson(db, R"({ "mods": { "A": ["other.archive","lut"] } })");
    FinalizeConflictDB(db);
    ASSERT_TRUE(db.mods.count("A"));
    EXPECT_EQ(db.mods.at("A").archives, (std::vector<std::string>{ "a.archive" }));
}

TEST(CuratedConflicts, MalformedJsonCountsParseErrorNoThrow) {
    ConflictDB db;
    EXPECT_FALSE(MergeConflictJson(db, "{ not valid json"));
    EXPECT_EQ(db.parseErrors, 1u);
    EXPECT_TRUE(db.mods.empty());
}

TEST(CuratedConflicts, UndefinedConflictMemberPruned) {
    auto db = Build({ R"({ "mods": { "A": "a.archive" }, "conflicts": [["A","Ghost"]] })" });
    auto r = EvaluateConflicts(db, { "a.archive" }, false);
    EXPECT_TRUE(r.active.empty());
}

TEST(CuratedConflicts, ThreeMemberPartialExceptionAllSurvive) {
    auto db = Build({ R"({ "mods": { "A": "a.archive", "B": "b.archive", "C": "c.archive" }, "conflicts": [["A","B","C"]], "exceptions": [["A","B"]] })" });
    auto r = EvaluateConflicts(db, { "a.archive", "b.archive", "c.archive" }, false);
    EXPECT_TRUE(HasGroup(r, { "A", "B", "C" }));
}

TEST(CuratedConflicts, ThreeMemberDoubleExceptionDropsBridgeMod) {
    auto db = Build({ R"({ "mods": { "A": "a.archive", "B": "b.archive", "C": "c.archive" }, "conflicts": [["A","B","C"]], "exceptions": [["A","B"],["A","C"]] })" });
    auto r = EvaluateConflicts(db, { "a.archive", "b.archive", "c.archive" }, false);
    EXPECT_TRUE(HasGroup(r, { "B", "C" }));
    bool aPresent = false;
    for (const auto& g : r.active) {
        for (const auto& m : g.mods) {
            if (m == "A") aPresent = true;
        }
    }
    EXPECT_FALSE(aPresent);
}

TEST(CuratedConflicts, PhantomLibertySkippedWhenEngineReadInvalid) {
    std::string j = R"({ "mods": { "A": "a.archive" }, "dependencies": { "A": ["Phantom Liberty"] } })";
    auto db = Build({ j });

    auto r1 = EvaluateConflicts(db, { "a.archive" }, false, false);
    EXPECT_TRUE(r1.missingDeps.empty());

    auto r2 = EvaluateConflicts(db, { "a.archive" }, false, true);
    ASSERT_EQ(r2.missingDeps.size(), 1u);
    EXPECT_EQ(r2.missingDeps[0].required, (std::vector<std::string>{ "Phantom Liberty" }));
}
