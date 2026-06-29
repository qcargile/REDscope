#include <Windows.h>
#include <RED4ext/RED4ext.hpp>
#include <filesystem>
#include <string>
#include <thread>
#include "Plugin.h"
#include "Logger.h"
#include "breadcrumbs/BreadcrumbApi.h"
#include "util/PreallocatedBuffer.h"
#include "handler/SehHandler.h"
#include "hooks/Hooks.h"
#include "rtti/HashDecoder.h"
#include "snapshot/EngineDumpEnricher.h"
#include "snapshot/RttiSnapshot.h"
#include "snapshot/SnapshotWorker.h"
#include "symbols/SymbolDb.h"
#include "symbols/SymbolFetch.h"

namespace {
    std::filesystem::path           g_pluginDir;
    redscope::Config                g_config;
    RED4ext::v1::PluginHandle       g_pluginHandle = nullptr;
    const RED4ext::v1::Sdk*         g_sdk          = nullptr;
}

namespace redscope {
    const std::filesystem::path& PluginDir() { return g_pluginDir; }
    const Config& GetConfig() { return g_config; }
    RED4ext::v1::PluginHandle PluginHandle() { return g_pluginHandle; }
    const RED4ext::v1::Sdk*   Sdk()          { return g_sdk; }
}

namespace {

void Boot(RED4ext::v1::PluginHandle aHandle, const RED4ext::v1::Sdk* aSdk) {
    using namespace redscope;
    g_pluginHandle = aHandle;
    g_sdk          = aSdk;

    wchar_t modulePath[MAX_PATH] = {};
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&Boot), &self);
    ::GetModuleFileNameW(self, modulePath, MAX_PATH);
    g_pluginDir = std::filesystem::path(modulePath).parent_path();

    log::Init(g_pluginDir.c_str());
    log::Info("REDscope " REDSCOPE_VERSION " loaded.");
    log::Info("Plugin dir: " + g_pluginDir.string());

    auto crashesDir = g_pluginDir / "crashes";
    redscope::MainCrashBuffer().Reserve(redscope::kMainCrashBufferBytes);
    redscope::SidecarBuffer().Reserve(redscope::kSidecarBufferBytes);
    redscope::handler::Arm(crashesDir);

    g_config = redscope::LoadConfig(g_pluginDir);
    log::Info("Config: snapshotIntervalMs=" + std::to_string(g_config.snapshotIntervalMs));
    redscope::handler::Configure(g_config.respectDebugger, g_config.terminateProcessAfterReport);

    uint32_t enriched = redscope::snap::EnrichRecentCrashesDefaultRoot(crashesDir);
    if (enriched > 0) {
        log::Info("Enriched " + std::to_string(enriched) +
                  " prior crash report(s) with REDEngine dump signals.");
    }

    redscope::rtti::InstallSdkHashResolvers();

    redscope::snap::RegisterRttiReadyCallback();

    redscope::snap::Start(g_config.snapshotIntervalMs);

    {
        HMODULE exe = ::GetModuleHandleW(L"Cyberpunk2077.exe");
        if (exe) {
            wchar_t exePath[MAX_PATH] = {};
            if (::GetModuleFileNameW(exe, exePath, MAX_PATH) > 0) {
                std::filesystem::path p(exePath);
                redscope::symbols::StartLoad(p.parent_path().parent_path().parent_path());
            }
        }
    }

    redscope::api::RegisterRedScriptBindings();

    redscope::hooks::InstallAll();

    if (g_config.symbolsAutoDownload) {
        redscope::symfetch::StartAutoDownloadAsync(g_pluginDir / "symbols");
    }

    if (g_config.testInjectCrashAfterSeconds > 0) {
        auto delay = g_config.testInjectCrashAfterSeconds;
        log::Warn("test-inject scheduled: deliberate NULL deref in "
                  + std::to_string(delay) + "s");
        std::thread([delay]{
            ::Sleep(delay * 1000);
            log::Warn("test-inject firing now");
            log::Flush();
            volatile int* p = nullptr;
            *p = 0xDEAD;
        }).detach();
    }
}

void Teardown() {
    using namespace redscope;
    log::Info("REDscope unloading.");
    redscope::hooks::UninstallAll();
    redscope::api::UnregisterRedScriptBindings();
    redscope::snap::Stop();
    redscope::handler::Uninstall();
    log::Shutdown();
    g_sdk          = nullptr;
    g_pluginHandle = nullptr;
}

}

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle,
                                        RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk) {
    switch (aReason) {
        case RED4ext::v1::EMainReason::Load:   Boot(aHandle, aSdk); break;
        case RED4ext::v1::EMainReason::Unload: Teardown();          break;
    }
    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo) {
    aInfo->name    = L"REDscope";
    aInfo->author  = L"Quentin";
    aInfo->version = RED4EXT_V1_SEMVER(0, 6, 0);
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_INDEPENDENT;
    aInfo->sdk     = RED4EXT_V1_SDK_VERSION_1_0_0_COMPAT_0_5_0;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() {
    return RED4EXT_API_VERSION_1_COMPAT_0;
}
