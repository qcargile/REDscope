#pragma once
#include <filesystem>
#include <cstdint>

namespace redscope {

struct Config {
    bool        respectDebugger = true;
    bool        terminateProcessAfterReport = false;
    bool        captureAllThreadStacks = true;
    uint32_t    maxCrashFiles = 50;

    uint32_t    findEntityRingSize = 32;
    uint32_t    tweakDbRingSize = 16;
    uint32_t    archiveRingSize = 16;
    uint32_t    snapshotIntervalMs = 500;

    std::filesystem::path symbolDbPath;
    bool        symbolsAutoDownload = false;

    uint32_t    stackDumpSlotCount = 512;

    uint32_t    testInjectCrashAfterSeconds = 0;

    bool        hangWatchdogEnabled  = true;
    uint32_t    hangThresholdSeconds = 45;
};

Config LoadConfig(const std::filesystem::path& pluginDir);

}
