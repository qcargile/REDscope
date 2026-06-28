#include <gtest/gtest.h>
#include "../src/snapshot/InstalledMods.h"
#include "../src/snapshot/Snapshot.h"
#include "../src/report/Sections.h"
#include "../src/util/PreallocatedBuffer.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using redscope::snap::EnumerateInstalled;
using redscope::snap::InstalledMod;
using redscope::snap::ModInventory;
using redscope::snap::ModManager;
using redscope::snap::ModType;

namespace redscope::snap {
std::filesystem::path TestMO2RootFromUsvfsPath(const wchar_t* usvfsPath);
}

namespace {

class InstalledModsTest : public ::testing::Test {
protected:
    fs::path base;
    fs::path gameRoot;
    fs::path mo2Root;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        char name[64];
        std::snprintf(name, sizeof(name), "redscope_installed_%llx",
                      (unsigned long long)gen());
        base = fs::temp_directory_path() / name;
        gameRoot = base / "game";
        mo2Root  = base / "mo2";
        fs::create_directories(gameRoot);
        fs::create_directories(mo2Root);
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

    static void Touch(const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
    }

    static bool HasMod(const ModInventory& inv, std::string_view name) {
        for (const auto& m : inv.mods) {
            if (m.name == name) return true;
        }
        return false;
    }

    static const InstalledMod* Find(const ModInventory& inv, std::string_view name) {
        for (const auto& m : inv.mods) {
            if (m.name == name) return &m;
        }
        return nullptr;
    }

    static size_t CountType(const ModInventory& inv, ModType t) {
        size_t n = 0;
        for (const auto& m : inv.mods) if (m.type == t) ++n;
        return n;
    }
};

}

TEST_F(InstalledModsTest, DetectsNoManagerNativeOnly) {
    Touch(gameRoot / "red4ext" / "plugins" / "foo" / "foo.dll");
    Touch(gameRoot / "bin" / "x64" / "plugins" / "cyber_engine_tweaks" / "mods" / "bar" / "init.lua");
    Touch(gameRoot / "archive" / "pc" / "mod" / "baz.archive");

    auto inv = EnumerateInstalled(gameRoot);

    EXPECT_EQ(inv.manager, ModManager::None);
    EXPECT_EQ(inv.truncated, 0u);
    EXPECT_TRUE(HasMod(inv, "foo"));
    EXPECT_TRUE(HasMod(inv, "bar"));
    EXPECT_TRUE(HasMod(inv, "baz"));

    const auto* foo = Find(inv, "foo");
    const auto* bar = Find(inv, "bar");
    const auto* baz = Find(inv, "baz");
    ASSERT_NE(foo, nullptr);
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(baz, nullptr);
    EXPECT_EQ(foo->type, ModType::Red4ext);
    EXPECT_EQ(bar->type, ModType::Cet);
    EXPECT_EQ(baz->type, ModType::Archive);
    EXPECT_TRUE(foo->enabled);
    EXPECT_TRUE(bar->enabled);
    EXPECT_TRUE(baz->enabled);
}

TEST_F(InstalledModsTest, DetectsNativeRedScriptAndTweakXLAndAsi) {
    Touch(gameRoot / "r6" / "scripts" / "CoolMod" / "logic.reds");
    Touch(gameRoot / "r6" / "tweaks" / "CoolTweak" / "foo.yaml");
    Touch(gameRoot / "r6" / "tweaks" / "top_level.yaml");
    Touch(gameRoot / "bin" / "x64" / "plugins" / "legacy.asi");

    auto inv = EnumerateInstalled(gameRoot);

    EXPECT_EQ(inv.manager, ModManager::None);
    const auto* reds = Find(inv, "CoolMod");
    const auto* tw   = Find(inv, "CoolTweak");
    const auto* top  = Find(inv, "top_level");
    const auto* asi  = Find(inv, "legacy");
    ASSERT_NE(reds, nullptr);
    ASSERT_NE(tw,   nullptr);
    ASSERT_NE(top,  nullptr);
    ASSERT_NE(asi,  nullptr);
    EXPECT_EQ(reds->type, ModType::RedScript);
    EXPECT_EQ(tw->type,   ModType::TweakXL);
    EXPECT_EQ(top->type,  ModType::TweakXL);
    EXPECT_EQ(asi->type,  ModType::Asi);
}

TEST_F(InstalledModsTest, ParsesMO2Modlist) {
    WriteText(mo2Root / "ModOrganizer.ini",
              "[General]\nselected_profile=TestProfile\n");
    WriteText(mo2Root / "profiles" / "TestProfile" / "modlist.txt",
              "+EnabledMod\n-DisabledMod\n#Separator_separator\n+SecondEnabled\n");

    auto inv = EnumerateInstalled(gameRoot, mo2Root);

    EXPECT_EQ(inv.manager, ModManager::MO2);
    ASSERT_EQ(inv.mods.size(), 3u);
    EXPECT_EQ(inv.mods[0].name, "EnabledMod");
    EXPECT_TRUE(inv.mods[0].enabled);
    EXPECT_EQ(inv.mods[1].name, "DisabledMod");
    EXPECT_FALSE(inv.mods[1].enabled);
    EXPECT_EQ(inv.mods[2].name, "SecondEnabled");
    EXPECT_TRUE(inv.mods[2].enabled);
    EXPECT_NE(inv.managerDetails.find("TestProfile"), std::string::npos);
}

TEST_F(InstalledModsTest, ParsesMO2ModlistWithByteArrayProfile) {
    WriteText(mo2Root / "ModOrganizer.ini",
              "[General]\nselected_profile=@ByteArray(Chrome & Blood - Standard Profile)\n");
    WriteText(mo2Root / "profiles" / "Chrome & Blood - Standard Profile" / "modlist.txt",
              "+AmpMod\n");

    auto inv = EnumerateInstalled(gameRoot, mo2Root);

    EXPECT_EQ(inv.manager, ModManager::MO2);
    ASSERT_EQ(inv.mods.size(), 1u);
    EXPECT_EQ(inv.mods[0].name, "AmpMod");
    EXPECT_NE(inv.managerDetails.find("Chrome & Blood - Standard Profile"),
              std::string::npos);
    EXPECT_EQ(inv.managerDetails.find("@ByteArray"), std::string::npos);
}

TEST_F(InstalledModsTest, MO2ProfileExistsButModlistMissing) {
    WriteText(mo2Root / "ModOrganizer.ini",
              "[General]\nselected_profile=Ghost\n");
    fs::create_directories(mo2Root / "profiles" / "Ghost");

    auto inv = EnumerateInstalled(gameRoot, mo2Root);

    EXPECT_EQ(inv.manager, ModManager::MO2);
    EXPECT_TRUE(inv.mods.empty());
    EXPECT_NE(inv.managerDetails.find("modlist.txt"), std::string::npos);
    EXPECT_NE(inv.managerDetails.find("Ghost"), std::string::npos);
}

TEST_F(InstalledModsTest, CollectMO2OverlayRelpathsEnabledOverwriteAndRootStrip) {
    WriteText(mo2Root / "ModOrganizer.ini",
              "[General]\nselected_profile=Default\n");
    WriteText(mo2Root / "profiles" / "Default" / "modlist.txt",
              "+ModA\n-ModB\n+ModC\n-Stuff_separator\n");
    Touch(mo2Root / "mods" / "ModA" / "r6" / "scripts" / "a.reds");
    Touch(mo2Root / "mods" / "ModB" / "r6" / "scripts" / "b.reds");
    Touch(mo2Root / "mods" / "ModC" / "Root" / "bin" / "x64" / "c.dll");
    Touch(mo2Root / "overwrite" / "r6" / "cache" / "d.bin");

    auto set = redscope::snap::CollectMO2OverlayRelpaths(mo2Root);
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(set->count("r6\\scripts\\a.reds"), 1u) << "enabled mod file is in the overlay set";
    EXPECT_EQ(set->count("r6\\scripts\\b.reds"), 0u) << "disabled mod is excluded";
    EXPECT_EQ(set->count("bin\\x64\\c.dll"), 1u) << "Root Builder Root/ prefix is stripped";
    EXPECT_EQ(set->count("r6\\cache\\d.bin"), 1u) << "the overwrite tree is included";
}

TEST_F(InstalledModsTest, CollectMO2OverlayRelpathsHonorsBaseDirectory) {
    fs::path baseDir = base / "mo2data";
    WriteText(mo2Root / "ModOrganizer.ini",
              "[General]\nselected_profile=Default\n[Settings]\nbase_directory=" + baseDir.string() + "\n");
    WriteText(mo2Root / "profiles" / "Default" / "modlist.txt", "+ModA\n");
    Touch(baseDir / "mods" / "ModA" / "r6" / "scripts" / "a.reds");
    Touch(baseDir / "overwrite" / "r6" / "cache" / "d.bin");

    auto set = redscope::snap::CollectMO2OverlayRelpaths(mo2Root);
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(set->count("r6\\scripts\\a.reds"), 1u) << "mods resolved under a custom base_directory";
    EXPECT_EQ(set->count("r6\\cache\\d.bin"), 1u) << "overwrite resolved under a custom base_directory";
}

TEST_F(InstalledModsTest, MO2PopulatesRed4extSubpath) {
    WriteText(mo2Root / "ModOrganizer.ini",
              "[General]\nselected_profile=P\n");
    WriteText(mo2Root / "profiles" / "P" / "modlist.txt", "+WeirdFolderName\n");
    Touch(mo2Root / "mods" / "WeirdFolderName" / "red4ext" / "plugins" /
          "RealDllName" / "RealDllName.dll");

    auto inv = EnumerateInstalled(gameRoot, mo2Root);

    ASSERT_EQ(inv.mods.size(), 1u);
    EXPECT_EQ(inv.mods[0].name, "WeirdFolderName");
    EXPECT_EQ(inv.mods[0].type, ModType::Red4ext);
    EXPECT_EQ(inv.mods[0].subpath, "red4ext/plugins/RealDllName/RealDllName.dll");
}

TEST_F(InstalledModsTest, MO2SingleProfileFallback) {
    WriteText(mo2Root / "ModOrganizer.ini", "[General]\n");
    WriteText(mo2Root / "profiles" / "OnlyProfile" / "modlist.txt",
              "+SoloMod\n");

    auto inv = EnumerateInstalled(gameRoot, mo2Root);

    EXPECT_EQ(inv.manager, ModManager::MO2);
    ASSERT_EQ(inv.mods.size(), 1u);
    EXPECT_EQ(inv.mods[0].name, "SoloMod");
}

TEST_F(InstalledModsTest, MO2UnresolvableProfileReturnsEmptyList) {
    WriteText(mo2Root / "ModOrganizer.ini", "garbage not an ini\n");

    auto inv = EnumerateInstalled(gameRoot, mo2Root);

    EXPECT_EQ(inv.manager, ModManager::MO2);
    EXPECT_TRUE(inv.mods.empty());
    EXPECT_FALSE(inv.managerDetails.empty());
}

TEST_F(InstalledModsTest, MO2RootWalkFindsRootWhenUsvfsAtRoot) {
    WriteText(mo2Root / "ModOrganizer.ini", "[General]\n");
    Touch(mo2Root / "usvfs_x64.dll");

    fs::path dllPath = mo2Root / "usvfs_x64.dll";
    fs::path found = redscope::snap::TestMO2RootFromUsvfsPath(dllPath.wstring().c_str());
    EXPECT_EQ(found, mo2Root);
}

TEST_F(InstalledModsTest, MO2RootWalkFindsRootWhenUsvfsInDllsSubdir) {
    WriteText(mo2Root / "ModOrganizer.ini", "[General]\n");
    Touch(mo2Root / "dlls" / "usvfs_x64.dll");

    fs::path dllPath = mo2Root / "dlls" / "usvfs_x64.dll";
    fs::path found = redscope::snap::TestMO2RootFromUsvfsPath(dllPath.wstring().c_str());
    EXPECT_EQ(found, mo2Root);
}

TEST_F(InstalledModsTest, MO2RootWalkReturnsEmptyWhenIniNotFound) {
    Touch(mo2Root / "usvfs_x64.dll");

    fs::path dllPath = mo2Root / "usvfs_x64.dll";
    fs::path found = redscope::snap::TestMO2RootFromUsvfsPath(dllPath.wstring().c_str());
    EXPECT_TRUE(found.empty());
}

TEST_F(InstalledModsTest, ParsesVortexManifest) {
    const char* manifest = R"({
  "files": [
    {"relPath": "red4ext\\plugins\\foo\\foo.dll", "source": "FooMod", "target": ""},
    {"relPath": "r6\\scripts\\foo.reds",          "source": "FooMod", "target": ""},
    {"relPath": "archive\\pc\\mod\\bar.archive",  "source": "BarMod", "target": ""}
  ]
})";
    WriteText(gameRoot / "__vortex_deployment.json", manifest);

    auto inv = EnumerateInstalled(gameRoot);

    EXPECT_EQ(inv.manager, ModManager::Vortex);
    ASSERT_EQ(inv.mods.size(), 2u);
    EXPECT_TRUE(HasMod(inv, "FooMod"));
    EXPECT_TRUE(HasMod(inv, "BarMod"));
    EXPECT_EQ(inv.mods[0].name, "BarMod");
    EXPECT_EQ(inv.mods[1].name, "FooMod");
}

TEST_F(InstalledModsTest, TruncationEnforced) {
    const size_t kOver = redscope::snap::kMaxInstalledMods + 100;
    for (size_t i = 0; i < kOver; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "mod_%05zu.archive", i);
        Touch(gameRoot / "archive" / "pc" / "mod" / name);
    }

    auto inv = EnumerateInstalled(gameRoot);

    EXPECT_EQ(inv.manager, ModManager::None);
    EXPECT_EQ(inv.mods.size(), redscope::snap::kMaxInstalledMods);
    EXPECT_EQ(inv.truncated, 100u);
}

TEST(EmitInstalledMods, EmptyInventoryPrintsPlaceholder) {
    redscope::PreallocatedBuffer buf;
    buf.Reserve(64 * 1024);

    redscope::report::EmitInstalledMods(buf);
    std::string out(buf.Data(), buf.Size());

    EXPECT_NE(out.find("Installed mods"), std::string::npos);
    EXPECT_NE(out.find("(no inventory available)"), std::string::npos);
}

namespace redscope::snap {
void TestSetCurrentSnapshot(const Snapshot* s) noexcept;
}

namespace {

class EmitInstalledModsFixture : public ::testing::Test {
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

    static void SetModuleName(redscope::Snapshot& snap, uint16_t idx, const char* name) {
        std::strncpy(snap.moduleNames[idx], name, redscope::kModuleNameCap - 1);
        snap.moduleNames[idx][redscope::kModuleNameCap - 1] = '\0';
    }
    static void AddLoadedModule(redscope::Snapshot& snap, const char* name) {
        uint32_t i = snap.moduleCount++;
        snap.modules[i].base = 0x10000000 + (uintptr_t)i * 0x10000;
        snap.modules[i].size = 0x1000;
        snap.modules[i].nameIndex    = (uint16_t)i;
        snap.modules[i].versionIndex = (uint16_t)i;
        SetModuleName(snap, (uint16_t)i, name);
    }
};

}

TEST_F(EmitInstalledModsFixture, SummaryCountsByType) {
    s.inventory.manager = redscope::snap::ModManager::None;

    redscope::snap::InstalledMod fooMod;
    fooMod.name    = "foo";
    fooMod.type    = redscope::snap::ModType::Red4ext;
    fooMod.enabled = true;
    s.inventory.mods.push_back(std::move(fooMod));

    redscope::snap::InstalledMod barMod;
    barMod.name    = "bar";
    barMod.type    = redscope::snap::ModType::Red4ext;
    barMod.enabled = true;
    s.inventory.mods.push_back(std::move(barMod));

    redscope::report::EmitInstalledMods(buf);
    std::string out(buf.Data(), buf.Size());

    EXPECT_NE(out.find("2 installed, 2 enabled"), std::string::npos);
    EXPECT_EQ(out.find("[loaded]"), std::string::npos);
}

TEST_F(EmitInstalledModsFixture, SummaryCountsEnabledAndManager) {
    s.inventory.manager = redscope::snap::ModManager::MO2;
    s.inventory.managerDetails = "profile 'X'";
    redscope::snap::InstalledMod mod;
    mod.name    = "OffMod";
    mod.type    = redscope::snap::ModType::Unknown;
    mod.enabled = false;
    s.inventory.mods.push_back(std::move(mod));

    redscope::report::EmitInstalledMods(buf);
    std::string out(buf.Data(), buf.Size());

    EXPECT_NE(out.find("1 installed, 0 enabled"), std::string::npos);
    EXPECT_NE(out.find("MO2 (profile 'X')"), std::string::npos);
    EXPECT_EQ(out.find("MO2 (MO2"), std::string::npos);
}

TEST_F(EmitInstalledModsFixture, GoldenStringFormat) {
    s.inventory.manager = redscope::snap::ModManager::MO2;
    s.inventory.managerDetails = "profile 'Test'";

    redscope::snap::InstalledMod foo;
    foo.name    = "foo";
    foo.version = "1.2.3";
    foo.type    = redscope::snap::ModType::Red4ext;
    foo.subpath = "red4ext/plugins/foo/foo.dll";
    foo.enabled = true;
    s.inventory.mods.push_back(std::move(foo));

    redscope::snap::InstalledMod bar;
    bar.name    = "bar";
    bar.type    = redscope::snap::ModType::Archive;
    bar.subpath = "archive/pc/mod/bar.archive";
    bar.enabled = false;
    s.inventory.mods.push_back(std::move(bar));

    redscope::report::EmitInstalledMods(buf);
    std::string out(buf.Data(), buf.Size());

    const char* expected =
        "--- Installed mods (summary; full per-mod list in .json sidecar) ---------------\n"
        "Manager: MO2 (profile 'Test')\n"
        "2 installed, 1 enabled\n"
        "Loaded DLLs are under 'Loaded modules'; full per-mod list (name / version / enabled) is in the .json sidecar.\n"
        "\n";
    EXPECT_EQ(out, expected);
}
