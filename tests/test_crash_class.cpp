#include <gtest/gtest.h>
#include "../src/report/CrashClass.h"

using redscope::report::ClassifyCrash;
using redscope::report::CrashClass;
using redscope::report::FrameView;
using redscope::report::IsRed4extModule;
using redscope::report::IsScriptRuntimeSymbol;

TEST(CrashClass, Red4extDllRecognisedAsScriptRuntime) {
    EXPECT_TRUE(IsRed4extModule("RED4ext.dll"));
    EXPECT_TRUE(IsRed4extModule("red4ext.dll"));
}

TEST(CrashClass, OrdinaryModulesAreNotScriptRuntime) {
    EXPECT_FALSE(IsRed4extModule("Cyberpunk2077.exe"));
    EXPECT_FALSE(IsRed4extModule("KERNEL32.DLL"));
    EXPECT_FALSE(IsRed4extModule("REDscope.dll"));
    EXPECT_FALSE(IsRed4extModule(""));
    EXPECT_FALSE(IsRed4extModule(nullptr));
}

TEST(CrashClass, ScriptRuntimeSymbolsRecognised) {
    EXPECT_TRUE(IsScriptRuntimeSymbol("CClass::ExecuteFunction"));
    EXPECT_TRUE(IsScriptRuntimeSymbol("CBaseFunction::Execute"));
    EXPECT_TRUE(IsScriptRuntimeSymbol("CScriptedFunction::Execute"));
    EXPECT_TRUE(IsScriptRuntimeSymbol("CStackFrame::Step"));
    EXPECT_TRUE(IsScriptRuntimeSymbol("ScriptGameInstance::CallScriptFunction"));
}

TEST(CrashClass, NonScriptSymbolsNotRecognised) {
    EXPECT_FALSE(IsScriptRuntimeSymbol("res::ResourceReference::EnsureLoaded"));
    EXPECT_FALSE(IsScriptRuntimeSymbol("DefaultAllocator::Free"));
    EXPECT_FALSE(IsScriptRuntimeSymbol(""));
    EXPECT_FALSE(IsScriptRuntimeSymbol(nullptr));
}

TEST(CrashClass, EmptyStackAndNoScriptDepthIsUnknown) {
    EXPECT_EQ(ClassifyCrash(nullptr, 0, 0), CrashClass::Unknown);
}

TEST(CrashClass, PureNativeStackIsNative) {
    FrameView frames[] = {
        { "Cyberpunk2077.exe", "res::ResourceReference::EnsureLoaded" },
        { "Cyberpunk2077.exe", "DefaultAllocator::Free" },
        { "KERNEL32.DLL", nullptr },
        { "ntdll.dll", nullptr },
    };
    EXPECT_EQ(ClassifyCrash(frames, 4, 0), CrashClass::Native);
}

TEST(CrashClass, Red4extFrameFlipsToScripted) {
    FrameView frames[] = {
        { "Cyberpunk2077.exe", "SomeNativeThing" },
        { "RED4ext.dll", nullptr },
        { "Cyberpunk2077.exe", nullptr },
    };
    EXPECT_EQ(ClassifyCrash(frames, 3, 0), CrashClass::Scripted);
}

TEST(CrashClass, ScriptRuntimeSymbolOnCp2077FrameFlipsToScripted) {
    FrameView frames[] = {
        { "Cyberpunk2077.exe", "CClass::ExecuteFunction" },
        { "KERNEL32.DLL", nullptr },
    };
    EXPECT_EQ(ClassifyCrash(frames, 2, 0), CrashClass::Scripted);
}

TEST(CrashClass, ScriptStackDepthAloneFlipsToScripted) {
    FrameView frames[] = {
        { "Cyberpunk2077.exe", "res::ResourceReference::EnsureLoaded" },
    };
    EXPECT_EQ(ClassifyCrash(frames, 1, 3), CrashClass::Scripted);
}

TEST(CrashClass, ScriptStackDepthWithNoFramesStillScripted) {
    EXPECT_EQ(ClassifyCrash(nullptr, 0, 5), CrashClass::Scripted);
}

TEST(CrashClass, CaseInsensitiveModuleNameMatch) {
    FrameView frames[] = {
        { "red4ext.DLL", nullptr },
    };
    EXPECT_EQ(ClassifyCrash(frames, 1, 0), CrashClass::Scripted);
}

TEST(CrashClass, NullSymbolsDoNotFlipClassification) {
    FrameView frames[] = {
        { "Cyberpunk2077.exe", nullptr },
        { "Cyberpunk2077.exe", nullptr },
    };
    EXPECT_EQ(ClassifyCrash(frames, 2, 0), CrashClass::Native);
}
