#include <gtest/gtest.h>
#include "../src/snapshot/PluginMetadata.h"
#include "../src/snapshot/Snapshot.h"
#include "../src/report/Sections.h"
#include "../src/util/PreallocatedBuffer.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using redscope::snap::EnumeratePluginMetadata;
using redscope::snap::PluginMetadata;

namespace redscope::snap {
void TestSetCurrentSnapshot(const Snapshot* s) noexcept;
}

namespace {

class PluginMetadataTest : public ::testing::Test {
protected:
    fs::path base;
    fs::path gameRoot;
    fs::path logsDir;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        char name[64];
        std::snprintf(name, sizeof(name), "redscope_plugmeta_%llx",
                      (unsigned long long)gen());
        base = fs::temp_directory_path() / name;
        gameRoot = base / "game";
        logsDir = gameRoot / "red4ext" / "logs";
        fs::create_directories(logsDir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    static void WriteText(const fs::path& p, std::string_view body) {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f.write(body.data(), (std::streamsize)body.size());
    }

    static void WriteCetLog(const fs::path& gameRoot, std::string_view body) {
        fs::path p = gameRoot / "bin" / "x64" / "plugins"
                     / "cyber_engine_tweaks" / "cyber_engine_tweaks.log";
        WriteText(p, body);
    }
};

constexpr std::string_view kRealisticLog =
    "[2026-04-17 19:47:11.757] [RED4ext] [info] 921876 game addresses loaded\n"
    "[2026-04-17 19:47:12.473] [RED4ext] [info] Loading plugin from 'D:\\SteamLibrary\\steamapps\\common\\Cyberpunk 2077\\red4ext\\plugins\\ArchiveXL\\ArchiveXL.dll'...\n"
    "[2026-04-17 19:47:12.716] [RED4ext] [info] ArchiveXL (version: 1.26.2, author(s): psiberx) has been loaded\n"
    "[2026-04-17 19:47:13.100] [RED4ext] [info] Loading plugin from 'D:\\SteamLibrary\\steamapps\\common\\Cyberpunk 2077\\red4ext\\plugins\\audioware\\audioware.dll'...\n"
    "[2026-04-17 19:47:13.575] [RED4ext] [info] audioware (version: 1.8.0-rc.0, author(s): Roms1383) has been loaded\n"
    "[2026-04-17 19:47:14.200] [RED4ext] [info] Loading plugin from 'D:\\SteamLibrary\\steamapps\\common\\Cyberpunk 2077\\red4ext\\plugins\\LootIconsExtensionFix\\LootIconsExtensionFix.dll'...\n"
    "[2026-04-17 19:47:14.440] [RED4ext] [info] LootIconsExtensionFix (version: 0.2.0, author(s): Demon9ne + Roms1383) has been loaded\n";

}

TEST_F(PluginMetadataTest, ParsesRealRed4extLog) {
    WriteText(logsDir / "red4ext-2026-04-17-19-47-11.log", kRealisticLog);

    PluginMetadata meta = EnumeratePluginMetadata(gameRoot);

    EXPECT_EQ(meta.source, PluginMetadata::Source::Red4extLog);
    EXPECT_EQ(meta.byDll.size(), 3u);
    EXPECT_EQ(meta.failedLoadCount, 0u);
    EXPECT_EQ(meta.logTimestamp, "2026-04-17 19:47:11");

    auto it = meta.byDll.find(std::string_view("archivexl.dll"));
    ASSERT_NE(it, meta.byDll.end());
    EXPECT_EQ(it->second.name, "ArchiveXL");
    EXPECT_EQ(it->second.version, "1.26.2");
    EXPECT_EQ(it->second.authors, "psiberx");
    EXPECT_EQ(it->second.dllBasename, "archivexl.dll");

    it = meta.byDll.find(std::string_view("audioware.dll"));
    ASSERT_NE(it, meta.byDll.end());
    EXPECT_EQ(it->second.name, "audioware");
    EXPECT_EQ(it->second.version, "1.8.0-rc.0");
    EXPECT_EQ(it->second.authors, "Roms1383");

    it = meta.byDll.find(std::string_view("looticonsextensionfix.dll"));
    ASSERT_NE(it, meta.byDll.end());
    EXPECT_EQ(it->second.version, "0.2.0");
    EXPECT_EQ(it->second.authors, "Demon9ne + Roms1383");
}

TEST_F(PluginMetadataTest, CountsFailedLoads) {
    constexpr std::string_view kLog =
        "[2026-04-17 19:47:12.473] [RED4ext] [info] Loading plugin from 'C:\\x\\foo.dll'...\n"
        "[2026-04-17 19:47:12.716] [RED4ext] [info] foo (version: 1.0.0, author(s): alice) has been loaded\n"
        "[2026-04-17 19:47:12.800] [RED4ext] [info] Loading plugin from 'C:\\x\\broken.dll'...\n"
        "[2026-04-17 19:47:12.900] [RED4ext] [error] Failed to load plugin 'broken.dll': bad image\n"
        "[2026-04-17 19:47:13.100] [RED4ext] [info] Loading plugin from 'C:\\x\\bar.dll'...\n"
        "[2026-04-17 19:47:13.575] [RED4ext] [info] bar (version: 2.0.0, author(s): bob) has been loaded\n";
    WriteText(logsDir / "red4ext-2026-04-17-19-47-11.log", kLog);

    PluginMetadata meta = EnumeratePluginMetadata(gameRoot);

    EXPECT_EQ(meta.byDll.size(), 2u);
    EXPECT_NE(meta.byDll.find(std::string_view("foo.dll")), meta.byDll.end());
    EXPECT_NE(meta.byDll.find(std::string_view("bar.dll")), meta.byDll.end());
    EXPECT_EQ(meta.byDll.find(std::string_view("broken.dll")), meta.byDll.end());
    EXPECT_EQ(meta.failedLoadCount, 1u);

    constexpr std::string_view kLogInfoFailure =
        "[ts] [RED4ext] [info] Loading plugin from 'C:\\x\\foo.dll'...\n"
        "[ts] [RED4ext] [info] foo (version: 1.0.0, author(s): alice) has been loaded\n"
        "[ts] [RED4ext] [info] Failed to load plugin 'broken.dll': missing dep\n";
    fs::remove_all(logsDir);
    fs::create_directories(logsDir);
    WriteText(logsDir / "red4ext-2026-04-17-19-47-11.log", kLogInfoFailure);

    meta = EnumeratePluginMetadata(gameRoot);
    EXPECT_EQ(meta.failedLoadCount, 1u);
}

TEST_F(PluginMetadataTest, HandlesAuthorsWithSpecialChars) {
    constexpr std::string_view kLog =
        "[2026-04-17 19:47:12.473] [RED4ext] [info] Loading plugin from 'C:\\x\\weird.dll'...\n"
        "[2026-04-17 19:47:12.716] [RED4ext] [info] Weird (version: 0.9.1-beta+build.42, author(s): Foo + Bar (someone) + Baz) has been loaded\n";
    WriteText(logsDir / "red4ext-2026-04-17-19-47-11.log", kLog);

    PluginMetadata meta = EnumeratePluginMetadata(gameRoot);

    ASSERT_EQ(meta.byDll.size(), 1u);
    auto it = meta.byDll.find(std::string_view("weird.dll"));
    ASSERT_NE(it, meta.byDll.end());
    EXPECT_EQ(it->second.version, "0.9.1-beta+build.42");
    EXPECT_EQ(it->second.authors, "Foo + Bar (someone) + Baz");
}

TEST_F(PluginMetadataTest, PicksNewestLog) {
    WriteText(logsDir / "red4ext-2026-04-15-10-00-00.log",
              "[2026-04-15 10:00:00.000] [RED4ext] [info] Loading plugin from 'C:\\old.dll'...\n"
              "[2026-04-15 10:00:00.100] [RED4ext] [info] old (version: 0.1.0, author(s): old) has been loaded\n");
    WriteText(logsDir / "red4ext-2026-04-17-19-47-11.log",
              "[2026-04-17 19:47:11.000] [RED4ext] [info] Loading plugin from 'C:\\new.dll'...\n"
              "[2026-04-17 19:47:11.100] [RED4ext] [info] new (version: 2.0.0, author(s): new) has been loaded\n");
    WriteText(logsDir / "red4ext-2026-04-16-12-00-00.log",
              "[2026-04-16 12:00:00.000] [RED4ext] [info] Loading plugin from 'C:\\mid.dll'...\n"
              "[2026-04-16 12:00:00.100] [RED4ext] [info] mid (version: 1.0.0, author(s): mid) has been loaded\n");

    PluginMetadata meta = EnumeratePluginMetadata(gameRoot);

    EXPECT_EQ(meta.byDll.size(), 1u);
    EXPECT_NE(meta.byDll.find(std::string_view("new.dll")), meta.byDll.end());
    EXPECT_EQ(meta.byDll.find(std::string_view("old.dll")), meta.byDll.end());
    EXPECT_EQ(meta.byDll.find(std::string_view("mid.dll")), meta.byDll.end());
}

TEST_F(PluginMetadataTest, MissingLogDirReturnsNoLogFound) {
    fs::remove_all(logsDir);
    PluginMetadata meta = EnumeratePluginMetadata(gameRoot);
    EXPECT_EQ(meta.source, PluginMetadata::Source::NoLogFound);
    EXPECT_TRUE(meta.byDll.empty());
}

TEST_F(PluginMetadataTest, ExtractsCetVersion) {
    WriteText(logsDir / "red4ext-2026-04-17-19-47-11.log",
              "[2026-04-17 19:47:11.000] [RED4ext] [info] Loading plugin from 'C:\\x.dll'...\n"
              "[2026-04-17 19:47:11.100] [RED4ext] [info] X (version: 1.0.0, author(s): x) has been loaded\n");
    WriteCetLog(gameRoot,
                "[2026-04-17 19:47:11 UTC-06:00] [info] [...] Cyber Engine Tweaks is starting...\n"
                "[2026-04-17 19:47:11 UTC-06:00] [info] [...] CET version v1.37.1 [HEAD]\n"
                "[2026-04-17 19:47:11 UTC-06:00] [info] [...] Game version 3.0.80.51928\n");

    PluginMetadata meta = EnumeratePluginMetadata(gameRoot);
    EXPECT_EQ(meta.cetVersion, "1.37.1");
}

TEST_F(PluginMetadataTest, CetMissingLogLeavesVersionEmpty) {
    WriteText(logsDir / "red4ext-2026-04-17-19-47-11.log",
              "[2026-04-17 19:47:11.000] [RED4ext] [info] 921876 game addresses loaded\n");
    PluginMetadata meta = EnumeratePluginMetadata(gameRoot);
    EXPECT_TRUE(meta.cetVersion.empty());
}

namespace {

class EmitCrossRefFixture : public ::testing::Test {
protected:
    redscope::Snapshot s{};
    redscope::PreallocatedBuffer buf;

    void SetUp() override {
        buf.Reserve(64 * 1024);
        redscope::snap::TestSetCurrentSnapshot(&s);
    }
    void TearDown() override {
        redscope::snap::TestSetCurrentSnapshot(nullptr);
    }
};

}

TEST_F(EmitCrossRefFixture, InstalledModsSummaryIgnoresPerModMetadata) {
    s.inventory.manager = redscope::snap::ModManager::None;
    redscope::snap::InstalledMod arx;
    arx.name    = "ArchiveXL";
    arx.type    = redscope::snap::ModType::Red4ext;
    arx.subpath = "red4ext/plugins/ArchiveXL/ArchiveXL.dll";
    arx.enabled = true;
    s.inventory.mods.push_back(std::move(arx));

    redscope::snap::PluginInfo info;
    info.name = "ArchiveXL";
    info.version = "1.26.2";
    info.authors = "psiberx";
    info.dllBasename = "archivexl.dll";
    s.pluginMeta.byDll.emplace("archivexl.dll", std::move(info));

    redscope::report::EmitInstalledMods(buf);
    std::string out(buf.Data(), buf.Size());

    EXPECT_NE(out.find("1 installed, 1 enabled"), std::string::npos);
    EXPECT_EQ(out.find("by psiberx"), std::string::npos);
    EXPECT_EQ(out.find("ArchiveXL"), std::string::npos);
}
