#include <gtest/gtest.h>
#include "../src/snapshot/EngineDumpEnricher.h"

using redscope::snap::ParseEngineDumpAttchText;
using redscope::snap::ParseSaveMetadataJson;
using redscope::snap::RenderEngineStateSidecar;
using redscope::snap::EngineDumpAttchFields;
using redscope::snap::SaveMetadataFields;

static const char* kSampleAttch =
R"(Registered crash info file...
InternalVersion: 3.0.5294808  P4CL: 9778208  Stream: //R6.Root/R6.Release
!!!CRASHED!!!
Error Reason: Unhandled exception
"uptimeSeconds":615
"stopThreadID":9132
"exceptionCode":0xC0000005
"Engine/Scripts/CompileScriptsSuccess@2516#TID=0":"false"
"Game/LoadingStage@27745853#TID=0":"Waiting for syst...#<truncated>#"
"Game/SessionDesc/IsLoadingSavedSession@27385381#TID=0":"true"
"Game/SessionDesc/WorldName@27385379#TID=0":"03_night_city"
"GlobalMode/IsGame@17#TID=0":"true"
"GlobalMode/IsClosing@5#TID=0":"false"
"Engine/OOM@4#TID=0":"false"
"Game/TransformAnimator/Components@27781874#TID=0":"442"
"Game/TransformAnimator/RunningComponents@27781873#TID=0":"320"
"Game/Population/Registered@27781944#TID=0":"4618"
"Game/Population/Attached@27781943#TID=0":"104"
"Game/Interactions/HotSpots/CurrentCount@27234575#TID=0":"432"
"Game/Interactions/HotSpots/MaxCount@27234576#TID=0":"434"
"Game/Interactions/UniqueLayersCurrentCount@27234577#TID=0":"2082"
"Game/TransactionSystem/NumberOfItemsInPlayersInventory@27442769#TID=0":"726"
"Game/TransactionSystem/NumberOfItemsInPlayersStash@27710338#TID=0":"11"
"Game/StatsSystem/NumberOfStatsBundles@27779432#TID=0":"1375"
"Gpu/Device/UsedMemoryMB@27781947#TID=0":"6898"
"Gpu/Device/TotalMemoryMB@2548#TID=0":"11997"
"Streaming/MountState@27413635#TID=0":"Mounted"
"Streaming/LastObserverPosition@27781818#TID=0":"[-1049, 1364, 6]"
"Engine/VersionWatermark@2509#TID=0":"3.0.5294808  P4CL: 9778208  Stream: //R6.Root/R6.Release"
)";

TEST(AttchParser, ExtractsScriptCompileFalse) {
    auto f = ParseEngineDumpAttchText(kSampleAttch);
    EXPECT_TRUE(f.hasData);
    EXPECT_TRUE(f.compileScriptsSuccessSet);
    EXPECT_FALSE(f.compileScriptsSuccess);
}

TEST(AttchParser, ExtractsLoadingState) {
    auto f = ParseEngineDumpAttchText(kSampleAttch);
    EXPECT_EQ(f.loadingStage, "Waiting for syst...#<truncated>#");
    EXPECT_TRUE(f.isLoadingSavedSessionSet);
    EXPECT_TRUE(f.isLoadingSavedSession);
    EXPECT_EQ(f.worldName, "03_night_city");
}

TEST(AttchParser, ExtractsEngineMode) {
    auto f = ParseEngineDumpAttchText(kSampleAttch);
    EXPECT_TRUE(f.isGameSet);
    EXPECT_TRUE(f.isGame);
    EXPECT_TRUE(f.isClosingSet);
    EXPECT_FALSE(f.isClosing);
    EXPECT_TRUE(f.engineOOMSet);
    EXPECT_FALSE(f.engineOOM);
}

TEST(AttchParser, ExtractsPopulationAndScene) {
    auto f = ParseEngineDumpAttchText(kSampleAttch);
    EXPECT_EQ(f.transformAnimatorComponents, 442u);
    EXPECT_EQ(f.transformAnimatorRunningComponents, 320u);
    EXPECT_EQ(f.populationRegistered, 4618u);
    EXPECT_EQ(f.populationAttached, 104u);
    EXPECT_EQ(f.hotSpotsCurrent, 432u);
    EXPECT_EQ(f.hotSpotsMax, 434u);
    EXPECT_EQ(f.interactionsUniqueLayersCurrent, 2082u);
}

TEST(AttchParser, ExtractsInventoryAndStats) {
    auto f = ParseEngineDumpAttchText(kSampleAttch);
    EXPECT_EQ(f.playerInventoryItems, 726u);
    EXPECT_EQ(f.playerStashItems, 11u);
    EXPECT_EQ(f.statsBundles, 1375u);
}

TEST(AttchParser, ExtractsGpuAndStreaming) {
    auto f = ParseEngineDumpAttchText(kSampleAttch);
    EXPECT_EQ(f.gpuUsedMemoryMB, 6898u);
    EXPECT_EQ(f.gpuTotalMemoryMB, 11997u);
    EXPECT_EQ(f.streamingMountState, "Mounted");
    EXPECT_EQ(f.streamingLastObserverPosition, "[-1049, 1364, 6]");
}

TEST(AttchParser, ExtractsUptimeAndThreadBareKeys) {
    auto f = ParseEngineDumpAttchText(kSampleAttch);
    EXPECT_EQ(f.uptimeSeconds, 615u);
    EXPECT_EQ(f.stopThreadId, 9132u);
}

TEST(AttchParser, EmptyInputNoData) {
    auto f = ParseEngineDumpAttchText("");
    EXPECT_FALSE(f.hasData);
}

TEST(AttchParser, GarbageInputNoData) {
    auto f = ParseEngineDumpAttchText("blah blah not a real file\nfoo:bar\n");
    EXPECT_FALSE(f.hasData);
}

static const char* kSampleMeta = R"({
    "RootType": "saveMetadataContainer",
    "Data": {
        "metadata": {
            "gameDefinition": "",
            "activeQuests": "6B577A51814903CA;E118A84B9942C8C4",
            "trackedQuest": "LocKey#12913",
            "locationName": "LocKey#10962",
            "playTime": 34152.0948051,
            "level": 47.0,
            "initialBuildID": "3.0.3276551  P4CL: 4218285  Stream: R6.Patch0Hotfix2 ",
            "buildID": "3.0.5294808  P4CL: 9778208  Stream: //R6.Root/R6.Release ",
            "saveVersion": 269,
            "gameVersion": 2310,
            "name": "QuickSave-0",
            "lifePath": "StreetKid",
            "difficulty": "Story",
            "isModded": true
        }
    }
})";

TEST(SaveMetaParser, ExtractsBuildIDs) {
    auto m = ParseSaveMetadataJson(kSampleMeta);
    EXPECT_TRUE(m.hasData);
    EXPECT_NE(m.buildID.find("5294808"), std::string::npos);
    EXPECT_NE(m.initialBuildID.find("3276551"), std::string::npos);
    EXPECT_NE(m.initialBuildID, m.buildID);
}

TEST(SaveMetaParser, ExtractsSaveDetails) {
    auto m = ParseSaveMetadataJson(kSampleMeta);
    EXPECT_EQ(m.saveName, "QuickSave-0");
    EXPECT_EQ(m.saveVersion, 269u);
    EXPECT_EQ(m.gameVersion, 2310u);
    EXPECT_EQ(m.playerLevel, 47u);
    EXPECT_EQ(m.lifePath, "StreetKid");
    EXPECT_EQ(m.difficulty, "Story");
    EXPECT_TRUE(m.isModdedSet);
    EXPECT_TRUE(m.isModded);
}

TEST(SaveMetaParser, EmptyJsonNoData) {
    auto m = ParseSaveMetadataJson("");
    EXPECT_FALSE(m.hasData);
}

TEST(SaveMetaParser, MalformedJsonNoData) {
    auto m = ParseSaveMetadataJson("{not json");
    EXPECT_FALSE(m.hasData);
}

TEST(SidecarRenderer, SurfacesCrossVersionWarning) {
    auto m = ParseSaveMetadataJson(kSampleMeta);
    EngineDumpAttchFields empty{};
    auto txt = RenderEngineStateSidecar(empty, m);
    EXPECT_NE(txt.find("CROSS-VERSION SAVE"), std::string::npos);
}

TEST(SidecarRenderer, ShowsCompileStatusWithoutEditorializing) {
    auto f = ParseEngineDumpAttchText(kSampleAttch);
    SaveMetadataFields empty{};
    auto txt = RenderEngineStateSidecar(f, empty);
    EXPECT_NE(txt.find("Scripts compiled OK"), std::string::npos);
    EXPECT_NE(txt.find("false"), std::string::npos);
    EXPECT_EQ(txt.find("RED FLAG"), std::string::npos);
    EXPECT_EQ(txt.find("redscript.log"), std::string::npos);
}

TEST(SidecarRenderer, EmptyInputsProducesPlaceholder) {
    EngineDumpAttchFields a{};
    SaveMetadataFields    m{};
    auto txt = RenderEngineStateSidecar(a, m);
    EXPECT_NE(txt.find("no extractable signals"), std::string::npos);
}

TEST(SidecarRenderer, ContainsKeyNumbers) {
    auto f = ParseEngineDumpAttchText(kSampleAttch);
    auto m = ParseSaveMetadataJson(kSampleMeta);
    auto txt = RenderEngineStateSidecar(f, m);
    EXPECT_NE(txt.find("442"), std::string::npos);
    EXPECT_NE(txt.find("4618"), std::string::npos);
    EXPECT_NE(txt.find("6898"), std::string::npos);
    EXPECT_NE(txt.find("03_night_city"), std::string::npos);
    EXPECT_NE(txt.find("QuickSave-0"), std::string::npos);
}
