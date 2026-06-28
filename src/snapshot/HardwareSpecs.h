#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace redscope::snap {

struct GpuAdapter {
    std::string name;
    std::string driverVersion;
    uint64_t    vramDedicatedBytes = 0;
    uint64_t    sharedSystemBytes  = 0;
};

struct HardwareSpecs {
    std::string cpuName;
    uint32_t    cpuBaseMHz = 0;
    uint32_t    cpuMaxTurboMHz = 0;
    uint32_t    cpuPhysicalCores = 0;
    uint32_t    cpuLogicalCores = 0;
    bool        cpuSupportsSse42  = false;
    bool        cpuSupportsAvx    = false;
    bool        cpuSupportsAvx2   = false;
    bool        cpuSupportsAvx512 = false;

    std::vector<GpuAdapter> gpus;

    uint64_t    ramTotalBytes = 0;
    uint64_t    ramAvailableBytes = 0;
    std::string ramType;
    uint32_t    ramSpeedMHz = 0;
    uint32_t    ramDimmCount = 0;
    std::string ramManufacturer;
    std::string ramPartNumber;

    uint64_t    pageFileTotalBytes = 0;
    uint64_t    pageFileAvailBytes = 0;

    std::string bootDriveFs;
    std::string bootDriveMediaType;

    char        gameDriveLetter = '\0';
    std::string gameDriveFs;
    std::string gameDriveMediaType;

    uint32_t    displayWidth     = 0;
    uint32_t    displayHeight    = 0;
    uint32_t    displayRefreshHz = 0;
    std::string displayHdrState;

    std::string osProductName;
    std::string osDisplayVersion;
    uint32_t    osMajor = 0;
    uint32_t    osMinor = 0;
    uint32_t    osBuild = 0;
    uint32_t    osUbr = 0;

    uint64_t    systemUptimeMs            = 0;
    uint64_t    processUptimeMs           = 0;
    uint64_t    processCreationFileTimeMs = 0;
    bool        processElevated = false;
    std::string commandLine;
};

std::string ClassifyDriveMedia(wchar_t driveLetter);

HardwareSpecs CaptureHardwareSpecs();

void ParseSmbiosAndFillRam(const uint8_t* blob, uint32_t blobLen, HardwareSpecs& s);

std::string TranslateNvidiaDriverVersion(std::string_view windowsVersion) noexcept;

}
