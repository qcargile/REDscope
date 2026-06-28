#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace redscope::snap {

struct EngineDumpAttchFields {
    bool         hasData                   = false;
    uint64_t     uptimeSeconds             = 0;
    uint32_t     stopThreadId              = 0;
    uint32_t     exceptionCode             = 0;
    bool         compileScriptsSuccess     = false;
    bool         compileScriptsSuccessSet  = false;
    std::string  loadingStage;
    bool         isLoadingSavedSession     = false;
    bool         isLoadingSavedSessionSet  = false;
    std::string  worldName;
    bool         isGame                    = false;
    bool         isGameSet                 = false;
    bool         isClosing                 = false;
    bool         isClosingSet              = false;
    bool         engineOOM                 = false;
    bool         engineOOMSet              = false;
    uint32_t     transformAnimatorComponents        = 0;
    uint32_t     transformAnimatorRunningComponents = 0;
    uint32_t     populationRegistered               = 0;
    uint32_t     populationAttached                 = 0;
    uint32_t     hotSpotsCurrent                    = 0;
    uint32_t     hotSpotsMax                        = 0;
    uint32_t     interactionsUniqueLayersCurrent    = 0;
    uint32_t     playerInventoryItems      = 0;
    uint32_t     playerStashItems          = 0;
    uint32_t     statsBundles              = 0;
    uint32_t     gpuUsedMemoryMB           = 0;
    uint32_t     gpuTotalMemoryMB          = 0;
    std::string  streamingMountState;
    std::string  streamingLastObserverPosition;
    std::string  engineVersionWatermark;
};

struct SaveMetadataFields {
    bool         hasData          = false;
    std::string  saveName;
    std::string  buildID;
    std::string  initialBuildID;
    bool         isModded         = false;
    bool         isModdedSet      = false;
    uint32_t     saveVersion      = 0;
    uint32_t     gameVersion      = 0;
    std::string  trackedQuest;
    std::string  locationName;
    double       playTime         = 0.0;
    uint32_t     playerLevel      = 0;
    std::string  lifePath;
    std::string  difficulty;
};

EngineDumpAttchFields ParseEngineDumpAttchText(std::string_view content) noexcept;

SaveMetadataFields ParseSaveMetadataJson(std::string_view json) noexcept;

std::filesystem::path FindReportQueueDir(
    const std::filesystem::path&                 reportQueueRoot,
    uint32_t                                     pid,
    std::chrono::system_clock::time_point        crashTime,
    std::chrono::seconds                         tolerance = std::chrono::seconds{90});

std::string RenderEngineStateSidecar(const EngineDumpAttchFields& attch,
                                     const SaveMetadataFields&    meta);

uint32_t EnrichRecentCrashes(const std::filesystem::path& crashesDir,
                             const std::filesystem::path& reportQueueRoot);

uint32_t EnrichRecentCrashesDefaultRoot(const std::filesystem::path& crashesDir);

}
