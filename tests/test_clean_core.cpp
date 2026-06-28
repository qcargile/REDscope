#include <gtest/gtest.h>
#include "CleanCore.h"

#include <simdjson.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

using namespace cleanslate;

TEST(CleanCore, ManagerAndClassNames) {
    EXPECT_STREQ(ManagerName(Manager::None), "None");
    EXPECT_STREQ(ManagerName(Manager::MO2), "MO2");
    EXPECT_STREQ(ManagerName(Manager::Vortex), "Vortex");
    EXPECT_STREQ(FileClassName(FileClass::Managed), "managed");
    EXPECT_STREQ(FileClassName(FileClass::ManagedInfra), "managed-infra");
    EXPECT_STREQ(FileClassName(FileClass::Loose), "loose");
    EXPECT_STREQ(FileClassName(FileClass::OutdatedOrphan), "orphan");
}

TEST(CleanCore, ToJsonIsCompleteAndParseable) {
    CleanReport r;
    r.ok = true;
    r.manager = Manager::Vortex;
    r.gameDir = "D:\\Games\\Cyberpunk 2077";
    r.managerDetail = "Vortex (manifest present)";
    r.scanned = 1874;
    r.managed = 1852;
    r.managedInfra = 20;
    r.loose = 2;
    r.orphan = 0;
    r.vortexWillRedeploy = true;
    r.findings.push_back({ "engine\\config\\platform\\pc\\rendering.ini", FileClass::Loose, "", "leftover" });
    r.findings.push_back({ "archive\\pc\\mod\\Foo.archive", FileClass::Loose, "Foo Mod", "untracked" });

    std::string js = ToJson(r);
    EXPECT_NE(js.find("\"scanned\": 1874"), std::string::npos);
    EXPECT_NE(js.find("\"managed\": 1852"), std::string::npos);
    EXPECT_NE(js.find("\"loose\": 2"), std::string::npos);

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    ASSERT_EQ(parser.parse(js).get(doc), simdjson::SUCCESS) << "ToJson output must be valid JSON";
    EXPECT_EQ(std::string_view(doc["manager"]), "Vortex");
    EXPECT_FALSE(bool(doc["clean"]));
    EXPECT_EQ(int64_t(doc["counts"]["scanned"]), 1874);
    simdjson::dom::array findings = doc["findings"];
    EXPECT_EQ(findings.size(), 2u);
}

TEST(CleanCore, ToJsonEmptyFindingsIsClean) {
    CleanReport r;
    r.ok = true;
    r.manager = Manager::MO2;
    std::string js = ToJson(r);
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    ASSERT_EQ(parser.parse(js).get(doc), simdjson::SUCCESS);
    EXPECT_TRUE(bool(doc["clean"]));
    EXPECT_EQ(simdjson::dom::array(doc["findings"]).size(), 0u);
}

TEST(CleanCore, ParseVortexManifestExtractsAndNormalizes) {
    auto tmp = std::filesystem::temp_directory_path() / L"cleanslate_test_manifest.json";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << R"({
  "deploymentMethod": "hardlink_activator",
  "gameId": "cyberpunk2077",
  "files": [
    { "relPath": "archive\\pc\\mod\\Foo.archive", "source": "Foo Mod", "time": 1 },
    { "relPath": "r6/scripts/Bar.reds", "source": "Bar Mod", "time": 2 }
  ]
})";
    }
    std::unordered_map<std::string, std::string> src;
    ASSERT_TRUE(ParseVortexManifest(tmp.wstring(), src));
    EXPECT_EQ(src.size(), 2u);
    EXPECT_EQ(src["archive\\pc\\mod\\foo.archive"], "Foo Mod");
    EXPECT_EQ(src["r6\\scripts\\bar.reds"], "Bar Mod");
    std::filesystem::remove(tmp);
}

TEST(CleanCore, ParseVortexManifestRejectsMissingAndGarbage) {
    std::unordered_map<std::string, std::string> src;
    EXPECT_FALSE(ParseVortexManifest(L"Z:\\nope\\does_not_exist.json", src));

    auto tmp = std::filesystem::temp_directory_path() / L"cleanslate_test_garbage.json";
    { std::ofstream f(tmp, std::ios::binary); f << "not json at all {{{"; }
    EXPECT_FALSE(ParseVortexManifest(tmp.wstring(), src));
    std::filesystem::remove(tmp);
}

namespace {
std::filesystem::path MakePurgeFixture(const wchar_t* name) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"bin\\x64", ec);
    { std::ofstream(root / L"bin\\x64\\Cyberpunk2077.exe", std::ios::binary) << "x"; }
    fs::create_directories(root / L"r6\\scripts", ec);
    { std::ofstream(root / L"r6\\scripts\\Leftover.reds", std::ios::binary) << "module L"; }
    return root;
}
}

TEST(CleanCore, ScanDetectsPurgedVortexViaV2077Footprint) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = MakePurgeFixture(L"cs_purged_v2077");
    fs::create_directories(root / L"V2077" / L"Load Order", ec);

    CleanReport r = Scan(root.wstring());
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.manager, Manager::Vortex) << "V2077 footprint should mark a purged Vortex install";
    EXPECT_GE(r.loose, 1u) << "the leftover .reds must be flagged after a purge, not reported clean";
    EXPECT_NE(r.managerDetail.find("purged"), std::string::npos);

    fs::remove_all(root, ec);
}

TEST(CleanCore, ScanTreatsEmptyManifestAsPurgedNotFlood) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = MakePurgeFixture(L"cs_empty_manifest");
    { std::ofstream(root / L"vortex.deployment.json", std::ios::binary) << "{ \"files\": [] }"; }

    CleanReport r = Scan(root.wstring());
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.manager, Manager::Vortex);
    EXPECT_GE(r.loose, 1u);
    EXPECT_EQ(r.managed, 0u) << "an empty manifest must not classify files as managed";

    fs::remove_all(root, ec);
}

TEST(CleanCore, ScanNoneManagerFlagsPhysicalLeftovers) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = MakePurgeFixture(L"cs_none_manager");

    CleanReport r = Scan(root.wstring());
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.manager, Manager::None);
    EXPECT_GE(r.loose, 1u) << "no manager + physical mod file = loose";

    fs::remove_all(root, ec);
}

TEST(CleanCore, ScanSkipsVanillaPlatformConfigButFlagsModAddedIni) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = fs::temp_directory_path() / L"cs_vanilla_config";
    fs::remove_all(root, ec);
    fs::create_directories(root / L"bin\\x64", ec);
    { std::ofstream(root / L"bin\\x64\\Cyberpunk2077.exe", std::ios::binary) << "x"; }
    fs::create_directories(root / L"engine\\config\\platform\\pc", ec);
    { std::ofstream(root / L"engine\\config\\platform\\pc\\rendering.ini", std::ios::binary) << "[LevelOfDetail]\nDecalsHideDistance = 40.0\n"; }
    { std::ofstream(root / L"engine\\config\\platform\\pc\\platformgameplay.ini", std::ios::binary) << "[UiInput]\n"; }
    { std::ofstream(root / L"engine\\config\\platform\\pc\\input_loader.ini", std::ios::binary) << "[mod]\n"; }

    CleanReport r = Scan(root.wstring());
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.loose, 1u) << "only the mod-added input_loader.ini is loose; the 2 vanilla files are skipped";
    ASSERT_EQ(r.findings.size(), 1u);
    EXPECT_NE(r.findings[0].relPath.find("input_loader.ini"), std::string::npos);

    fs::remove_all(root, ec);
}

TEST(CleanCore, PlanFullCleanRemovesNonVanillaKeepsVanillaAndRedmod) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = fs::temp_directory_path() / L"cs_fullclean_plan";
    fs::remove_all(root, ec);
    fs::create_directories(root / L"bin\\x64", ec);
    { std::ofstream(root / L"bin\\x64\\Cyberpunk2077.exe", std::ios::binary) << "x"; }
    fs::create_directories(root / L"archive\\pc\\mod", ec);
    { std::ofstream(root / L"archive\\pc\\mod\\Foo.archive", std::ios::binary) << "A"; }
    fs::create_directories(root / L"red4ext\\plugins", ec);
    { std::ofstream(root / L"red4ext\\plugins\\Bar.dll", std::ios::binary) << "B"; }
    fs::create_directories(root / L"r6\\scripts", ec);
    fs::create_directories(root / L"r6\\config", ec);
    { std::ofstream(root / L"r6\\config\\options.json", std::ios::binary) << "{}"; }
    fs::create_directories(root / L"mods\\SomeMod", ec);
    { std::ofstream(root / L"mods\\.stub", std::ios::binary) << ""; }
    fs::create_directories(root / L"V2077\\Load Order", ec);
    { std::ofstream(root / L"vortex.deployment.json", std::ios::binary) << "{}"; }
    { std::ofstream(root / L"bin\\x64\\winmm.dll", std::ios::binary) << "L"; }
    fs::create_directories(root / L"engine\\config\\platform\\pc", ec);
    { std::ofstream(root / L"engine\\config\\platform\\pc\\rendering.ini", std::ios::binary) << "[x]"; }
    { std::ofstream(root / L"engine\\config\\platform\\pc\\modtweak.ini", std::ios::binary) << "[m]"; }

    FullCleanReport p = PlanFullClean(root.wstring());
    ASSERT_TRUE(p.ok);

    auto has = [&](const char* rel) {
        for (const auto& it : p.items) if (it.relPath == rel) return true;
        return false;
    };
    EXPECT_TRUE(has("archive\\pc\\mod"));
    EXPECT_TRUE(has("red4ext"));
    EXPECT_TRUE(has("r6\\scripts"));
    EXPECT_TRUE(has("mods\\SomeMod"));
    EXPECT_TRUE(has("V2077"));
    EXPECT_TRUE(has("vortex.deployment.json"));
    EXPECT_TRUE(has("bin\\x64\\winmm.dll"));
    EXPECT_TRUE(has("engine\\config\\platform\\pc\\modtweak.ini"));

    EXPECT_FALSE(has("mods\\.stub")) << "REDmod folder-keeper stays";
    EXPECT_FALSE(has("r6\\config")) << "vanilla r6 subdir stays";
    EXPECT_FALSE(has("engine\\config\\platform\\pc\\rendering.ini")) << "vanilla config stays";

    fs::remove_all(root, ec);
}

TEST(CleanCore, ProbeReferenceDistinguishesInsideOutsideOpenFail) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = fs::temp_directory_path() / L"cs_probe_ref";
    fs::remove_all(root, ec);
    fs::create_directories(root / L"red4ext\\plugins\\REDscope", ec);
    fs::path inside = root / L"red4ext\\plugins\\REDscope\\REDscope.dll";
    { std::ofstream(inside, std::ios::binary) << "dll"; }

    fs::path outsideDir = fs::temp_directory_path() / L"cs_probe_ref_mods";
    fs::remove_all(outsideDir, ec);
    fs::create_directories(outsideDir, ec);
    fs::path outside = outsideDir / L"REDscope.dll";
    { std::ofstream(outside, std::ios::binary) << "dll"; }

    fs::path rootC = fs::canonical(root, ec);
    if (ec) { rootC = root; ec.clear(); }

    EXPECT_EQ(ProbeReference(inside.wstring(), rootC.wstring()), ReferenceVerdict::ResolvedInsideRoot)
        << "a known-managed file resolving INSIDE gameRoot is the USVFS inversion signature -> suppress";
    EXPECT_EQ(ProbeReference(outside.wstring(), rootC.wstring()), ReferenceVerdict::ResolvedOutsideRoot)
        << "the healthy case: a USVFS-redirected file resolves OUTSIDE gameRoot -> trust the discriminator";
    EXPECT_EQ(ProbeReference((root / L"red4ext\\plugins\\REDscope\\nope.dll").wstring(), rootC.wstring()),
              ReferenceVerdict::OpenFailed)
        << "a missing canary must be OpenFailed, not silently treated as outside/healthy";

    fs::remove_all(root, ec);
    fs::remove_all(outsideDir, ec);
}

TEST(CleanCore, ScanOverlaySetFlagsOnlyFilesNotInTheManifest) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = fs::temp_directory_path() / L"cs_overlay_diff";
    fs::remove_all(root, ec);
    fs::create_directories(root / L"bin\\x64", ec);
    { std::ofstream(root / L"bin\\x64\\Cyberpunk2077.exe", std::ios::binary) << "x"; }
    fs::create_directories(root / L"r6\\scripts\\ModA", ec);
    fs::create_directories(root / L"r6\\scripts\\Leftover", ec);
    { std::ofstream(root / L"r6\\scripts\\ModA\\a.reds", std::ios::binary) << "module A"; }
    { std::ofstream(root / L"r6\\scripts\\Leftover\\x.reds", std::ios::binary) << "module X"; }

    std::unordered_set<std::string> overlay = { "r6\\scripts\\moda\\a.reds" };
    CleanReport r = Scan(root.wstring(), &overlay);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.managed, 1u) << "the overlaid mod file is managed via the manifest diff";
    EXPECT_EQ(r.loose, 1u) << "only the file absent from the overlay manifest is loose";
    ASSERT_EQ(r.findings.size(), 1u);
    EXPECT_NE(r.findings[0].relPath.find("Leftover"), std::string::npos)
        << "the non-overlaid file is the loose one, never the overlaid mod file";
}
