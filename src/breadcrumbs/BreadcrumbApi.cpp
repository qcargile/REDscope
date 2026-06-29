#include "BreadcrumbApi.h"
#include "CrashArchive.h"
#include "../Logger.h"
#include "../Plugin.h"
#include "../snapshot/GameStateLive.h"
#include "../snapshot/SnapshotWorker.h"
#include "../snapshot/Snapshot.h"
#include "../report/JsonSidecar.h"
#include "../util/PreallocatedBuffer.h"
#include "../symbols/SymbolFetch.h"
#include "../Config.h"
#include <vector>

#include <RED4ext/CName.hpp>
#include <RED4ext/CString.hpp>
#include <RED4ext/RTTISystem.hpp>
#include <RED4ext/Scripting/Functions.hpp>
#include <RED4ext/Scripting/Stack.hpp>
#include <RED4ext/Scripting/Utils.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace redscope::api {
namespace {

RED4ext::CGlobalFunction* g_registeredFn2 = nullptr;
RED4ext::CGlobalFunction* g_registeredFn3 = nullptr;
RED4ext::CGlobalFunction* g_registeredFn4 = nullptr;
RED4ext::CGlobalFunction* g_registeredFn5 = nullptr;
RED4ext::CGlobalFunction* g_registeredFn6 = nullptr;
RED4ext::CGlobalFunction* g_registeredFn7 = nullptr;
RED4ext::CGlobalFunction* g_registeredFn8 = nullptr;
RED4ext::CGlobalFunction* g_registeredFn9 = nullptr;


std::string SlurpFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <typename Pred>
std::filesystem::path FindLatestCrashFile(Pred pred) {
    auto dir = redscope::PluginDir() / "crashes";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return {};
    std::filesystem::path latest;
    std::filesystem::file_time_type latestTime{};
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto& p = entry.path();
        if (!pred(p)) continue;
        auto t = std::filesystem::last_write_time(p, ec);
        if (ec) continue;
        if (latest.empty() || t > latestTime) { latest = p; latestTime = t; }
    }
    return latest;
}

std::string ReadLatestCrashJson() {
    try {
        auto latest = FindLatestCrashFile([](const std::filesystem::path& p) {
            return p.extension() == ".json" && p.stem().extension() == ".crash";
        });
        if (latest.empty()) return {};
        return SlurpFile(latest);
    } catch (...) {
        return {};
    }
}

void GetLatestCrashJson_Native(RED4ext::IScriptable* aContext,
                               RED4ext::CStackFrame* aFrame,
                               void* aOut,
                               int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;
    std::string content = ReadLatestCrashJson();
    if (aOut) {
        *reinterpret_cast<RED4ext::CString*>(aOut) = RED4ext::CString(content.c_str());
    }
}

void GetAllCrashesJson_Native(RED4ext::IScriptable* aContext,
                              RED4ext::CStackFrame* aFrame,
                              void* aOut,
                              int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;
    std::string content = ReadAllCrashesJson(redscope::PluginDir() / "crashes", 1000);
    if (aOut) {
        *reinterpret_cast<RED4ext::CString*>(aOut) = RED4ext::CString(content.c_str());
    }
}

std::string ReadDiagnosticJson() {
    redscope::PreallocatedBuffer buf;
    buf.Reserve(512 * 1024);
    const redscope::Snapshot* snap = redscope::snap::Current();
    redscope::report::BuildDiagnosticJson(snap, buf);
    return std::string(buf.Data(), buf.Size());
}

void GetDiagnosticJson_Native(RED4ext::IScriptable* aContext,
                              RED4ext::CStackFrame* aFrame,
                              void* aOut,
                              int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;
    std::string content = ReadDiagnosticJson();
    if (aOut) {
        *reinterpret_cast<RED4ext::CString*>(aOut) = RED4ext::CString(content.c_str());
    }
}

std::string ReadLatestCrashReport() {
    try {
        auto latest = FindLatestCrashFile([](const std::filesystem::path& p) {
            return p.extension() == ".crash";
        });
        if (latest.empty()) return {};
        return SlurpFile(latest);
    } catch (...) {
        return {};
    }
}

std::string ReadLatestCrashSibling(const char* siblingExt) {
    try {
        auto latestCrash = FindLatestCrashFile([](const std::filesystem::path& p) {
            return p.extension() == ".crash";
        });
        if (latestCrash.empty()) return {};
        auto siblingPath = latestCrash;
        siblingPath.replace_extension(siblingExt);
        std::error_code ec;
        if (!std::filesystem::exists(siblingPath, ec)) return {};
        return SlurpFile(siblingPath);
    } catch (...) {
        return {};
    }
}

void GetLatestCrashReport_Native(RED4ext::IScriptable* aContext,
                                 RED4ext::CStackFrame* aFrame,
                                 void* aOut,
                                 int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;
    std::string content = ReadLatestCrashReport();
    if (!content.empty()) {
        std::string state = ReadLatestCrashSibling(".engine-state.txt");
        if (!state.empty()) {
            content += "\n--- engine-state.txt (sibling) ---\n";
            content += state;
        }
    }
    if (aOut) {
        *reinterpret_cast<RED4ext::CString*>(aOut) = RED4ext::CString(content.c_str());
    }
}

void SetState_Native(RED4ext::IScriptable* aContext,
                     RED4ext::CStackFrame* aFrame,
                     void* aOut,
                     int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(aOut);
    RED4EXT_UNUSED_PARAMETER(a4);

    RED4ext::CString key;
    RED4ext::CString value;
    RED4ext::GetParameter(aFrame, &key);
    RED4ext::GetParameter(aFrame, &value);
    aFrame->code++;

    redscope::snap::SetLiveState(key.c_str(), value.c_str());
}

void WriteAutoDownloadIni(bool enabled) {
    auto iniPath = redscope::PluginDir() / "REDscope.ini";
    std::vector<std::string> lines;
    {
        std::ifstream in(iniPath);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    const std::string newLine = std::string("auto_download = ") + (enabled ? "true" : "false");
    std::string section;
    bool done = false;
    for (auto& l : lines) {
        size_t a = l.find_first_not_of(" \t");
        std::string s = (a == std::string::npos) ? std::string() : l.substr(a);
        if (!s.empty() && s[0] == '[') {
            size_t e = s.find(']');
            section = (e != std::string::npos) ? s.substr(1, e - 1) : std::string();
            continue;
        }
        if (section == "symbols" && s.rfind("auto_download", 0) == 0) { l = newLine; done = true; }
    }
    if (!done) { lines.push_back("[symbols]"); lines.push_back(newLine); }
    std::ofstream out(iniPath, std::ios::trunc | std::ios::binary);
    for (auto& l : lines) out << l << "\n";
}

void DownloadSymbolsNow_Native(RED4ext::IScriptable* aContext, RED4ext::CStackFrame* aFrame, void* aOut, int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(aOut);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;
    redscope::symfetch::StartAutoDownloadAsync(redscope::PluginDir() / "symbols");
}

void SetAutoDownload_Native(RED4ext::IScriptable* aContext, RED4ext::CStackFrame* aFrame, void* aOut, int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(aOut);
    RED4EXT_UNUSED_PARAMETER(a4);
    bool enabled = false;
    RED4ext::GetParameter(aFrame, &enabled);
    aFrame->code++;
    WriteAutoDownloadIni(enabled);
}

void GetAutoDownload_Native(RED4ext::IScriptable* aContext, RED4ext::CStackFrame* aFrame, void* aOut, int64_t a4) {
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;
    bool v = redscope::LoadConfig(redscope::PluginDir()).symbolsAutoDownload;
    if (aOut) *reinterpret_cast<bool*>(aOut) = v;
}

void PostRegisterTypes() {
    auto* rtti = RED4ext::CRTTISystem::Get();
    if (!rtti) {
        log::Error("REDscope: CRTTISystem::Get() returned null; native bindings skipped.");
        return;
    }

    auto fn2 = RED4ext::CGlobalFunction::Create("REDscope.GetLatestCrashJson", "GetLatestCrashJson", &GetLatestCrashJson_Native);
    if (!fn2) {
        log::Error("REDscope.GetLatestCrashJson: CGlobalFunction::Create returned null; binding skipped.");
        return;
    }
    fn2->flags.isNative = 1;
    fn2->flags.isStatic = 1;
    fn2->SetReturnType("String");
    rtti->RegisterFunction(fn2);
    g_registeredFn2 = fn2;
    log::Info("REDscope.GetLatestCrashJson registered as native global function.");

    auto fn3 = RED4ext::CGlobalFunction::Create("REDscope.GetAllCrashesJson", "GetAllCrashesJson", &GetAllCrashesJson_Native);
    if (!fn3) {
        log::Error("REDscope.GetAllCrashesJson: CGlobalFunction::Create returned null; binding skipped.");
        return;
    }
    fn3->flags.isNative = 1;
    fn3->flags.isStatic = 1;
    fn3->SetReturnType("String");
    rtti->RegisterFunction(fn3);
    g_registeredFn3 = fn3;
    log::Info("REDscope.GetAllCrashesJson registered as native global function.");

    auto fn4 = RED4ext::CGlobalFunction::Create("REDscope.GetLatestCrashReport", "GetLatestCrashReport", &GetLatestCrashReport_Native);
    if (!fn4) {
        log::Error("REDscope.GetLatestCrashReport: CGlobalFunction::Create returned null; binding skipped.");
        return;
    }
    fn4->flags.isNative = 1;
    fn4->flags.isStatic = 1;
    fn4->SetReturnType("String");
    rtti->RegisterFunction(fn4);
    g_registeredFn4 = fn4;
    log::Info("REDscope.GetLatestCrashReport registered as native global function.");

    auto fn5 = RED4ext::CGlobalFunction::Create("REDscope.SetState", "SetState", &SetState_Native);
    if (!fn5) {
        log::Error("REDscope.SetState: CGlobalFunction::Create returned null; binding skipped.");
        return;
    }
    fn5->flags.isNative = 1;
    fn5->flags.isStatic = 1;
    fn5->SetReturnType("Void");
    fn5->AddParam("String", "key");
    fn5->AddParam("String", "value");
    rtti->RegisterFunction(fn5);
    g_registeredFn5 = fn5;
    log::Info("REDscope.SetState registered as native global function.");

    auto fn6 = RED4ext::CGlobalFunction::Create("REDscope.GetDiagnosticJson", "GetDiagnosticJson", &GetDiagnosticJson_Native);
    if (!fn6) {
        log::Error("REDscope.GetDiagnosticJson: CGlobalFunction::Create returned null; binding skipped.");
        return;
    }
    fn6->flags.isNative = 1;
    fn6->flags.isStatic = 1;
    fn6->SetReturnType("String");
    rtti->RegisterFunction(fn6);
    g_registeredFn6 = fn6;
    log::Info("REDscope.GetDiagnosticJson registered as native global function.");

    auto fn7 = RED4ext::CGlobalFunction::Create("REDscope.DownloadSymbolsNow", "DownloadSymbolsNow", &DownloadSymbolsNow_Native);
    if (!fn7) {
        log::Error("REDscope.DownloadSymbolsNow: CGlobalFunction::Create returned null; binding skipped.");
        return;
    }
    fn7->flags.isNative = 1;
    fn7->flags.isStatic = 1;
    fn7->SetReturnType("Void");
    rtti->RegisterFunction(fn7);
    g_registeredFn7 = fn7;
    log::Info("REDscope.DownloadSymbolsNow registered as native global function.");

    auto fn8 = RED4ext::CGlobalFunction::Create("REDscope.SetAutoDownload", "SetAutoDownload", &SetAutoDownload_Native);
    if (!fn8) {
        log::Error("REDscope.SetAutoDownload: CGlobalFunction::Create returned null; binding skipped.");
        return;
    }
    fn8->flags.isNative = 1;
    fn8->flags.isStatic = 1;
    fn8->SetReturnType("Void");
    fn8->AddParam("Bool", "enabled");
    rtti->RegisterFunction(fn8);
    g_registeredFn8 = fn8;
    log::Info("REDscope.SetAutoDownload registered as native global function.");

    auto fn9 = RED4ext::CGlobalFunction::Create("REDscope.GetAutoDownload", "GetAutoDownload", &GetAutoDownload_Native);
    if (!fn9) {
        log::Error("REDscope.GetAutoDownload: CGlobalFunction::Create returned null; binding skipped.");
        return;
    }
    fn9->flags.isNative = 1;
    fn9->flags.isStatic = 1;
    fn9->SetReturnType("Bool");
    rtti->RegisterFunction(fn9);
    g_registeredFn9 = fn9;
    log::Info("REDscope.GetAutoDownload registered as native global function.");
}

}

void RegisterRedScriptBindings() {
    auto* rtti = RED4ext::CRTTISystem::Get();
    if (!rtti) {
        log::Error("RegisterRedScriptBindings: CRTTISystem::Get() returned null.");
        return;
    }
    rtti->AddPostRegisterCallback(&PostRegisterTypes);
}

void UnregisterRedScriptBindings() {
    if (auto* rtti = RED4ext::CRTTISystem::Get()) {
        if (g_registeredFn2) rtti->UnregisterFunction(g_registeredFn2);
        if (g_registeredFn3) rtti->UnregisterFunction(g_registeredFn3);
        if (g_registeredFn4) rtti->UnregisterFunction(g_registeredFn4);
        if (g_registeredFn5) rtti->UnregisterFunction(g_registeredFn5);
        if (g_registeredFn6) rtti->UnregisterFunction(g_registeredFn6);
        if (g_registeredFn7) rtti->UnregisterFunction(g_registeredFn7);
        if (g_registeredFn8) rtti->UnregisterFunction(g_registeredFn8);
        if (g_registeredFn9) rtti->UnregisterFunction(g_registeredFn9);
    }
    g_registeredFn2 = nullptr;
    g_registeredFn3 = nullptr;
    g_registeredFn4 = nullptr;
    g_registeredFn5 = nullptr;
    g_registeredFn6 = nullptr;
    g_registeredFn7 = nullptr;
    g_registeredFn8 = nullptr;
    g_registeredFn9 = nullptr;
}

}
