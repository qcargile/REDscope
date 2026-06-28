#include <gtest/gtest.h>
#include "../src/snapshot/WrapChain.h"
#include "../src/snapshot/InstalledMods.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using redscope::snap::BuildMethodKey;
using redscope::snap::CaptureWrapChains;
using redscope::snap::FindChain;
using redscope::snap::InstalledMod;
using redscope::snap::kMaxWrapLayers;
using redscope::snap::kWrapMethodKeyCap;
using redscope::snap::ModInventory;
using redscope::snap::ParseFuncDeclaration;
using redscope::snap::ParseWrapAnnotation;
using redscope::snap::WrapChainTable;

namespace {

class WrapChainTest : public ::testing::Test {
protected:
    fs::path gameRoot;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        char name[64];
        std::snprintf(name, sizeof(name), "redscope_wrapchain_%llx",
                      (unsigned long long)gen());
        gameRoot = fs::temp_directory_path() / name;
        fs::create_directories(gameRoot / "r6" / "scripts");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(gameRoot, ec);
    }

    void WriteScript(const fs::path& rel, const std::string& content) {
        auto p = gameRoot / "r6" / "scripts" / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f.write(content.data(), (std::streamsize)content.size());
    }
};

ModInventory Inv(std::vector<std::string> names) {
    ModInventory inv;
    for (auto& n : names) {
        InstalledMod m;
        m.name = std::move(n);
        inv.mods.push_back(std::move(m));
    }
    return inv;
}

}

TEST(ParseWrapAnnotation, ParsesStandardClassOnly) {
    std::string c;
    ASSERT_TRUE(ParseWrapAnnotation("@wrapMethod(PlayerPuppet)", c));
    EXPECT_EQ(c, "PlayerPuppet");
}

TEST(ParseWrapAnnotation, ToleratesWhitespace) {
    std::string c;
    ASSERT_TRUE(ParseWrapAnnotation("   @wrapMethod ( AccessPointControllerPS )  ", c));
    EXPECT_EQ(c, "AccessPointControllerPS");
}

TEST(ParseWrapAnnotation, RejectsDottedForm) {
    std::string c;
    EXPECT_FALSE(ParseWrapAnnotation("@wrapMethod(Foo.Bar)", c));
}

TEST(ParseWrapAnnotation, RejectsInvalidIdentifiers) {
    std::string c;
    EXPECT_FALSE(ParseWrapAnnotation("@wrapMethod(Foo-Bar)", c));
    EXPECT_FALSE(ParseWrapAnnotation("@wrapMethod()", c));
    EXPECT_FALSE(ParseWrapAnnotation("@wrapMethod(Foo bar)", c));
}

TEST(ParseWrapAnnotation, RejectsAddMethodAndReplaceMethod) {
    std::string c;
    EXPECT_FALSE(ParseWrapAnnotation("@addMethod(PlayerPuppet)",     c));
    EXPECT_FALSE(ParseWrapAnnotation("@replaceMethod(PlayerPuppet)", c));
}

TEST(ParseFuncDecl, ParsesPlainFunc) {
    std::string m;
    ASSERT_TRUE(ParseFuncDeclaration("func HasTag() -> Bool {", m));
    EXPECT_EQ(m, "HasTag");
}

TEST(ParseFuncDecl, ParsesPublicProtectedPrivateModifiers) {
    std::string m;
    ASSERT_TRUE(ParseFuncDeclaration("public func A() {", m));
    EXPECT_EQ(m, "A");
    ASSERT_TRUE(ParseFuncDeclaration("protected cb func OnGameAttached() -> Bool {", m));
    EXPECT_EQ(m, "OnGameAttached");
    ASSERT_TRUE(ParseFuncDeclaration("private final const func HasPersonalLinkSlot() -> Bool {", m));
    EXPECT_EQ(m, "HasPersonalLinkSlot");
}

TEST(ParseFuncDecl, RejectsMissingParen) {
    std::string m;
    EXPECT_FALSE(ParseFuncDeclaration("let x = func_helper(y);", m));
    EXPECT_FALSE(ParseFuncDeclaration("public func", m));
}

TEST(ParseFuncDecl, RejectsWordBoundaryFalsePositive) {
    std::string m;
    EXPECT_FALSE(ParseFuncDeclaration("refunc thing(y)", m));
}

TEST(BuildMethodKeyFn, ConcatenatesWithDot) {
    char buf[32];
    BuildMethodKey("Foo", "Bar", buf, sizeof(buf));
    EXPECT_STREQ(buf, "Foo.Bar");
}

TEST(BuildMethodKeyFn, TruncatesAtCapacity) {
    char buf[8];
    BuildMethodKey("LongClassName", "MethodName", buf, sizeof(buf));
    EXPECT_LE(std::strlen(buf), sizeof(buf) - 1);
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

TEST_F(WrapChainTest, EmptyGameRootYieldsEmptyTable) {
    auto t = CaptureWrapChains(fs::path{}, Inv({}));
    EXPECT_FALSE(t.scanPerformed);
    EXPECT_EQ   (t.chains.size(), 0u);
}

TEST_F(WrapChainTest, MissingScriptsDirStillMarksScanFalse) {
    std::error_code ec;
    fs::remove_all(gameRoot / "r6" / "scripts", ec);
    auto t = CaptureWrapChains(gameRoot, Inv({}));
    EXPECT_FALSE(t.scanPerformed);
}

TEST_F(WrapChainTest, RealSyntaxSingleWrap) {
    WriteScript("MBN/Breach.reds",
                "module MBN\n"
                "\n"
                "@wrapMethod(AccessPointControllerPS)\n"
                "public func FinalizeNetrunnerDive(state: HackingMinigameState) -> Void {\n"
                "  wrappedMethod(state);\n"
                "}\n");

    auto t = CaptureWrapChains(gameRoot, Inv({"MBN"}));
    EXPECT_TRUE(t.scanPerformed);
    ASSERT_EQ  (t.chains.size(),         1u);
    EXPECT_EQ  (t.totalLayers,   1u);
    EXPECT_EQ  (t.modsWithWraps, 1u);

    const auto& c = t.chains[0];
    EXPECT_STREQ(c.methodKey,           "AccessPointControllerPS.FinalizeNetrunnerDive");
    ASSERT_EQ   (c.layerCount,          1u);
    EXPECT_STREQ(c.layers[0].modName,   "MBN");
    EXPECT_NE   (std::strstr(c.layers[0].relFile, "Breach.reds"), nullptr);
    EXPECT_EQ   (c.layers[0].lineNumber, 3u);
}

TEST_F(WrapChainTest, FuncSkipsLeadingBlanksAndLineComments) {
    WriteScript("Mod/a.reds",
                "@wrapMethod(Foo)\n"
                "\n"
                "// guard against nulls\n"
                "public func Bar() -> Bool { return true; }\n");

    auto t = CaptureWrapChains(gameRoot, Inv({"Mod"}));
    ASSERT_EQ(t.chains.size(), 1u);
    EXPECT_STREQ(t.chains[0].methodKey, "Foo.Bar");
}

TEST_F(WrapChainTest, MultipleModsWrapSameMethodChainInAlphabeticalOrder) {
    WriteScript("Zeta/late.reds",
                "@wrapMethod(Foo)\nfunc Bar() {}\n");
    WriteScript("Alpha/early.reds",
                "@wrapMethod(Foo)\nfunc Bar() {}\n");
    WriteScript("Mid/mid.reds",
                "@wrapMethod(Foo)\nfunc Bar() {}\n");

    auto t = CaptureWrapChains(gameRoot, Inv({"Alpha", "Mid", "Zeta"}));
    ASSERT_EQ(t.chains.size(), 1u);
    const auto& c = t.chains[0];
    ASSERT_EQ(c.layerCount, 3u);
    EXPECT_STREQ(c.layers[0].modName, "Alpha");
    EXPECT_STREQ(c.layers[1].modName, "Mid");
    EXPECT_STREQ(c.layers[2].modName, "Zeta");
}

TEST_F(WrapChainTest, SeparateMethodsInSameFile) {
    WriteScript("Mod/a.reds",
                "@wrapMethod(Foo)\n"
                "public func Bar() {}\n"
                "\n"
                "@wrapMethod(Baz)\n"
                "public func Qux() {}\n");

    auto t = CaptureWrapChains(gameRoot, Inv({"Mod"}));
    EXPECT_EQ(t.chains.size(),       2u);
    EXPECT_EQ(t.totalLayers, 2u);
    EXPECT_NE(FindChain(t, "Foo.Bar"),  nullptr);
    EXPECT_NE(FindChain(t, "Baz.Qux"),  nullptr);
}

TEST_F(WrapChainTest, CommentedAnnotationsAreIgnored) {
    WriteScript("Mod/a.reds",
                "// @wrapMethod(Ghost)\n"
                "// public func Phantom() {}\n"
                "@wrapMethod(Real)\n"
                "public func Method() {}\n"
                "//  @wrapMethod(AlsoGhost)\n");

    auto t = CaptureWrapChains(gameRoot, Inv({"Mod"}));
    EXPECT_EQ(t.chains.size(), 1u);
    EXPECT_NE(FindChain(t, "Real.Method"),      nullptr);
    EXPECT_EQ(FindChain(t, "Ghost.Phantom"),    nullptr);
    EXPECT_EQ(FindChain(t, "AlsoGhost.Method"), nullptr);
}

TEST_F(WrapChainTest, BlockCommentedAnnotationIsIgnored) {
    WriteScript("Mod/a.reds",
                "/*\n"
                "  @wrapMethod(Inside)\n"
                "  public func Block() {}\n"
                "*/\n"
                "@wrapMethod(Real)\n"
                "public func Method() {}\n");

    auto t = CaptureWrapChains(gameRoot, Inv({"Mod"}));
    EXPECT_EQ(t.chains.size(), 1u);
    EXPECT_NE(FindChain(t, "Real.Method"),   nullptr);
    EXPECT_EQ(FindChain(t, "Inside.Block"),  nullptr);
}

TEST_F(WrapChainTest, ModAttributionPrefersInventoryNameCase) {
    WriteScript("betternetrunning/breach.reds",
                "@wrapMethod(AP)\n"
                "public func RefreshSlaves() {}\n");

    auto t = CaptureWrapChains(gameRoot, Inv({"Much Better Netrunning", "BetterNetrunning"}));
    ASSERT_EQ(t.chains.size(), 1u);
    EXPECT_STREQ(t.chains[0].layers[0].modName, "BetterNetrunning");
}

TEST_F(WrapChainTest, FindChainReturnsNullptrForMiss) {
    WriteScript("Mod/a.reds",
                "@wrapMethod(Real)\npublic func Method() {}\n");
    auto t = CaptureWrapChains(gameRoot, Inv({"Mod"}));
    EXPECT_EQ(FindChain(t, "Nope.Absent"),  nullptr);
    EXPECT_EQ(FindChain(t, nullptr),        nullptr);
    EXPECT_NE(FindChain(t, "Real.Method"),  nullptr);
}

TEST_F(WrapChainTest, DotlessPathInLeafAttributesToFileStem) {
    WriteScript("bare_file.reds",
                "@wrapMethod(Class)\n"
                "public func Method() {}\n");
    auto t = CaptureWrapChains(gameRoot, Inv({}));
    ASSERT_EQ(t.chains.size(), 1u);
    EXPECT_STREQ(t.chains[0].layers[0].modName, "bare_file");
}

TEST_F(WrapChainTest, ExtraLayersCounterCapturesOverflow) {
    for (size_t i = 0; i < kMaxWrapLayers + 2; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "Mod%zu/a.reds", i);
        WriteScript(name, "@wrapMethod(Foo)\npublic func Bar() {}\n");
    }
    auto t = CaptureWrapChains(gameRoot, Inv({}));
    ASSERT_EQ(t.chains.size(), 1u);
    const auto& c = t.chains[0];
    EXPECT_EQ(c.layerCount,  kMaxWrapLayers);
    EXPECT_EQ(c.extraLayers, 2u);
    EXPECT_EQ(t.totalLayers, kMaxWrapLayers + 2u);
}

TEST_F(WrapChainTest, OrphanedAnnotationResetsAfterNonFunc) {
    WriteScript("Mod/a.reds",
                "@wrapMethod(Orphan)\n"
                "let x: Int32 = 0;\n"
                "\n"
                "public func Unrelated() {}\n");
    auto t = CaptureWrapChains(gameRoot, Inv({"Mod"}));
    EXPECT_EQ(t.chains.size(), 0u);
}
