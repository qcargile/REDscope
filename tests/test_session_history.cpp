#include <gtest/gtest.h>
#include "../src/snapshot/SessionHistory.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using redscope::snap::ComputeModDiff;
using redscope::snap::ModDiff;
using redscope::snap::ModSnapshot;
using redscope::snap::PriorSession;
using redscope::snap::ReadSessionPrevious;
using redscope::snap::WriteSessionPrevious;

namespace {

class SessionHistoryTest : public ::testing::Test {
protected:
    fs::path base;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        char name[64];
        std::snprintf(name, sizeof(name), "redscope_sesshist_%llx",
                      (unsigned long long)gen());
        base = fs::temp_directory_path() / name;
        fs::create_directories(base);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    void WriteRaw(const std::string& name, const std::string& content) {
        std::ofstream f(base / name, std::ios::binary);
        f.write(content.data(), (std::streamsize)content.size());
    }
};

ModSnapshot M(const char* n, const char* v) {
    return { n, v };
}

}

TEST_F(SessionHistoryTest, ReadMissingFileReturnsNotPresent) {
    auto p = ReadSessionPrevious(base / "session.previous");
    EXPECT_FALSE(p.present);
    EXPECT_FALSE(p.parsedOk);
}

TEST_F(SessionHistoryTest, ReadBadMagicReturnsNotParsed) {
    WriteRaw("session.previous", "NOT_THE_MAGIC\nMod A|1.0\n");
    auto p = ReadSessionPrevious(base / "session.previous");
    EXPECT_TRUE(p.present);
    EXPECT_FALSE(p.parsedOk);
}

TEST_F(SessionHistoryTest, WriteAndReadRoundTrip) {
    std::vector<ModSnapshot> mods = { M("Zeta", "2.0"), M("Alpha", "1.0") };
    ASSERT_TRUE(WriteSessionPrevious(base / "session.previous", 1700000000, mods));

    auto p = ReadSessionPrevious(base / "session.previous");
    ASSERT_TRUE(p.present);
    ASSERT_TRUE(p.parsedOk);
    EXPECT_EQ(p.timestampUnix, 1700000000);
    ASSERT_EQ(p.mods.size(),    2u);
    EXPECT_EQ(p.mods[0].name,    "Alpha");
    EXPECT_EQ(p.mods[0].version, "1.0");
    EXPECT_EQ(p.mods[1].name,    "Zeta");
    EXPECT_EQ(p.mods[1].version, "2.0");
}

TEST_F(SessionHistoryTest, WriteOverwritesPriorContents) {
    ASSERT_TRUE(WriteSessionPrevious(base / "session.previous", 1, { M("Old", "0") }));
    ASSERT_TRUE(WriteSessionPrevious(base / "session.previous", 2, { M("New", "9") }));
    auto p = ReadSessionPrevious(base / "session.previous");
    ASSERT_EQ(p.mods.size(), 1u);
    EXPECT_EQ(p.mods[0].name, "New");
    EXPECT_EQ(p.timestampUnix, 2);
}

TEST_F(SessionHistoryTest, DiffEmptyWhenIdentical) {
    std::vector<ModSnapshot> same = { M("A", "1"), M("B", "2") };
    auto d = ComputeModDiff(same, same);
    EXPECT_TRUE(d.added.empty());
    EXPECT_TRUE(d.removed.empty());
    EXPECT_TRUE(d.updated.empty());
}

TEST_F(SessionHistoryTest, DiffDetectsAdditions) {
    auto d = ComputeModDiff({ M("A", "1") }, { M("A", "1"), M("B", "2") });
    ASSERT_EQ(d.added.size(),   1u);
    EXPECT_EQ(d.added[0].name,  "B");
    EXPECT_EQ(d.removed.size(), 0u);
    EXPECT_EQ(d.updated.size(), 0u);
}

TEST_F(SessionHistoryTest, DiffDetectsRemovals) {
    auto d = ComputeModDiff({ M("A", "1"), M("B", "2") }, { M("A", "1") });
    ASSERT_EQ(d.removed.size(),   1u);
    EXPECT_EQ(d.removed[0].name,  "B");
    EXPECT_EQ(d.added.size(),     0u);
}

TEST_F(SessionHistoryTest, DiffDetectsVersionUpdates) {
    auto d = ComputeModDiff({ M("A", "1.0") }, { M("A", "1.1") });
    ASSERT_EQ(d.updated.size(), 1u);
    EXPECT_EQ(d.updated[0].first.version,  "1.0");
    EXPECT_EQ(d.updated[0].second.version, "1.1");
    EXPECT_EQ(d.added.size(),   0u);
    EXPECT_EQ(d.removed.size(), 0u);
}

TEST_F(SessionHistoryTest, DiffIsOrderIndependent) {
    auto d = ComputeModDiff({ M("Zeta", "1"), M("Alpha", "1") },
                            { M("Alpha", "1"), M("Zeta", "1") });
    EXPECT_EQ(d.added.size(),   0u);
    EXPECT_EQ(d.removed.size(), 0u);
    EXPECT_EQ(d.updated.size(), 0u);
}

TEST_F(SessionHistoryTest, DiffMixed) {
    std::vector<ModSnapshot> prior   = { M("A", "1"), M("B", "1"), M("C", "1") };
    std::vector<ModSnapshot> current = { M("A", "2"),              M("C", "1"), M("D", "1") };
    auto d = ComputeModDiff(prior, current);
    ASSERT_EQ(d.added.size(),   1u);
    EXPECT_EQ(d.added[0].name,  "D");
    ASSERT_EQ(d.removed.size(), 1u);
    EXPECT_EQ(d.removed[0].name, "B");
    ASSERT_EQ(d.updated.size(), 1u);
    EXPECT_EQ(d.updated[0].first.name,     "A");
    EXPECT_EQ(d.updated[0].first.version,  "1");
    EXPECT_EQ(d.updated[0].second.version, "2");
}

TEST_F(SessionHistoryTest, DiffSkipsEmptyNames) {
    auto d = ComputeModDiff({ M("A", "1"), M("", "x") }, { M("A", "1"), M("", "y") });
    EXPECT_TRUE(d.added.empty());
    EXPECT_TRUE(d.removed.empty());
    EXPECT_TRUE(d.updated.empty());
}
