#include "HardwareSpecs.h"
#include "Snapshot.h"
#include "SnapshotWorker.h"
#include "../report/Sections.h"
#include "../util/PreallocatedBuffer.h"
#include "../util/StringUtils.h"
#include <windows.h>
#include <winioctl.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <powrprof.h>
#include <powerbase.h>
#include <wingdi.h>
#include <intrin.h>
#include <charconv>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "User32.lib")

struct REDSCOPE_PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

namespace redscope::snap {
namespace {

using Microsoft::WRL::ComPtr;

std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out((size_t)(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

void RTrim(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
}

std::string ReadRegStringHKLM(const wchar_t* subkey, const wchar_t* value) {
    HKEY h = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &h)
        != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0, bytes = 0;
    LONG r = ::RegQueryValueExW(h, value, nullptr, &type, nullptr, &bytes);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) {
        ::RegCloseKey(h);
        return {};
    }
    std::wstring wbuf(bytes / sizeof(wchar_t) + 1, L'\0');
    r = ::RegQueryValueExW(h, value, nullptr, &type,
                           reinterpret_cast<LPBYTE>(wbuf.data()), &bytes);
    ::RegCloseKey(h);
    if (r != ERROR_SUCCESS) return {};
    while (!wbuf.empty() && wbuf.back() == L'\0') wbuf.pop_back();
    std::string s = WideToUtf8(wbuf.c_str());
    RTrim(s);
    return s;
}

bool ReadRegDwordHKLM(const wchar_t* subkey, const wchar_t* value, uint32_t& out) {
    HKEY h = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &h)
        != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, data = 0, bytes = sizeof(data);
    LONG r = ::RegQueryValueExW(h, value, nullptr, &type,
                                reinterpret_cast<LPBYTE>(&data), &bytes);
    ::RegCloseKey(h);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return false;
    out = (uint32_t)data;
    return true;
}

void FillCpuFeatureFlags(HardwareSpecs& s) {
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 1);
    s.cpuSupportsSse42 = (regs[2] & (1 << 20)) != 0;
    s.cpuSupportsAvx   = (regs[2] & (1 << 28)) != 0;

    int regs7[4] = {0, 0, 0, 0};
    __cpuidex(regs7, 7, 0);
    s.cpuSupportsAvx2   = (regs7[1] & (1 << 5))  != 0;
    s.cpuSupportsAvx512 = (regs7[1] & (1 << 16)) != 0;
}

void FillCpuMaxTurbo(HardwareSpecs& s) {
    if (s.cpuLogicalCores == 0) return;
    std::vector<REDSCOPE_PROCESSOR_POWER_INFORMATION> info(s.cpuLogicalCores);
    ULONG bytes = (ULONG)(info.size() * sizeof(info[0]));
    LONG rc = ::CallNtPowerInformation(
        (POWER_INFORMATION_LEVEL)11, nullptr, 0, info.data(), bytes);
    if (rc != 0) return;
    ULONG maxMhz = 0;
    for (auto& e : info) {
        if (e.MaxMhz > maxMhz) maxMhz = e.MaxMhz;
    }
    s.cpuMaxTurboMHz = (uint32_t)maxMhz;
}

void FillCpu(HardwareSpecs& s) {
    s.cpuName = ReadRegStringHKLM(
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString");

    uint32_t mhz = 0;
    if (ReadRegDwordHKLM(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                         L"~MHz", mhz)) {
        s.cpuBaseMHz = mhz;
    }

    DWORD bytes = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (bytes > 0) {
        std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[bytes]);
        if (buf) {
            auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.get());
            if (::GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes)) {
                uint32_t physical = 0, logical = 0;
                DWORD offset = 0;
                while (offset < bytes) {
                    auto* rec = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                        buf.get() + offset);
                    if (rec->Relationship == RelationProcessorCore) {
                        ++physical;
                        for (WORD g = 0; g < rec->Processor.GroupCount; ++g) {
                            KAFFINITY mask = rec->Processor.GroupMask[g].Mask;
                            while (mask) { logical += (uint32_t)(mask & 1u); mask >>= 1; }
                        }
                    }
                    offset += rec->Size;
                }
                s.cpuPhysicalCores = physical;
                s.cpuLogicalCores  = logical;
            }
        }
    }

    FillCpuFeatureFlags(s);
    FillCpuMaxTurbo(s);
}

std::string FormatUmdVersion(IDXGIAdapter* adapter) {
    LARGE_INTEGER umd{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) return {};
    uint16_t aa = (uint16_t)((umd.HighPart >> 16) & 0xFFFF);
    uint16_t bb = (uint16_t)(umd.HighPart & 0xFFFF);
    uint16_t cc = (uint16_t)((umd.LowPart  >> 16) & 0xFFFF);
    uint16_t dd = (uint16_t)(umd.LowPart & 0xFFFF);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", aa, bb, cc, dd);
    return buf;
}

void FillGpu(HardwareSpecs& s) {
    ComPtr<IDXGIFactory> factory;
    if (FAILED(::CreateDXGIFactory(IID_PPV_ARGS(&factory)))) return;

    struct AdapterKey {
        std::wstring desc;
        UINT vendorId;
        UINT deviceId;
        UINT subSysId;
        SIZE_T vram;
        bool operator==(const AdapterKey& o) const {
            return vendorId == o.vendorId && deviceId == o.deviceId
                && subSysId == o.subSysId && vram == o.vram && desc == o.desc;
        }
    };
    std::vector<AdapterKey> seen;
    for (UINT idx = 0; ; ++idx) {
        ComPtr<IDXGIAdapter> candidate;
        if (factory->EnumAdapters(idx, &candidate) != S_OK) break;
        DXGI_ADAPTER_DESC cdesc{};
        if (FAILED(candidate->GetDesc(&cdesc))) continue;
        if (cdesc.VendorId == 0x1414) continue;
        AdapterKey key{cdesc.Description, cdesc.VendorId, cdesc.DeviceId,
                       cdesc.SubSysId, cdesc.DedicatedVideoMemory};
        bool dup = false;
        for (auto& existing : seen) {
            if (existing == key) { dup = true; break; }
        }
        if (dup) continue;
        seen.push_back(std::move(key));

        GpuAdapter a;
        a.name = WideToUtf8(cdesc.Description);
        RTrim(a.name);
        a.driverVersion     = FormatUmdVersion(candidate.Get());
        a.vramDedicatedBytes = cdesc.DedicatedVideoMemory;
        a.sharedSystemBytes  = cdesc.SharedSystemMemory;
        s.gpus.push_back(std::move(a));
    }
}

std::string SmbiosString(const uint8_t* formatted, uint8_t formattedLen,
                         const uint8_t* end, uint8_t index) {
    if (index == 0) return {};
    const uint8_t* cursor = formatted + formattedLen;
    uint8_t wanted = index;
    while (cursor < end) {
        const uint8_t* strStart = cursor;
        while (cursor < end && *cursor != 0) ++cursor;
        if (strStart == cursor) return {};
        if (--wanted == 0) {
            return std::string(reinterpret_cast<const char*>(strStart),
                               (size_t)(cursor - strStart));
        }
        ++cursor;
    }
    return {};
}

const char* SmbiosMemoryTypeName(uint8_t t) {
    switch (t) {
        case 0x18: return "DDR3";
        case 0x19: return "FBD2";
        case 0x1A: return "DDR4";
        case 0x1B: return "LPDDR";
        case 0x1C: return "LPDDR2";
        case 0x1D: return "LPDDR3";
        case 0x1E: return "LPDDR4";
        case 0x22: return "DDR5";
        case 0x23: return "LPDDR5";
        default:   return "";
    }
}

inline uint16_t SmbiosReadU16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

void FillRam(HardwareSpecs& s) {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (::GlobalMemoryStatusEx(&m)) {
        s.ramTotalBytes      = m.ullTotalPhys;
        s.ramAvailableBytes  = m.ullAvailPhys;
        s.pageFileTotalBytes = m.ullTotalPageFile;
        s.pageFileAvailBytes = m.ullAvailPageFile;
    }

    const DWORD provider = 'RSMB';
    DWORD size = ::GetSystemFirmwareTable(provider, 0, nullptr, 0);
    if (size == 0) return;
    std::vector<uint8_t> buf((size_t)size);
    DWORD got = ::GetSystemFirmwareTable(provider, 0, buf.data(), size);
    if (got == 0 || got > size) return;
    ParseSmbiosAndFillRam(buf.data(), got, s);
}

std::string QueryDriveFilesystem(wchar_t driveLetter) {
    wchar_t root[4] = { driveLetter, L':', L'\\', L'\0' };
    wchar_t fs[MAX_PATH] = {};
    if (::GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr,
                                fs, MAX_PATH)) {
        return WideToUtf8(fs);
    }
    return {};
}

wchar_t ResolveGameDriveLetter() {
    HMODULE exe = ::GetModuleHandleW(L"Cyberpunk2077.exe");
    if (!exe) return L'\0';
    wchar_t path[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameW(exe, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L'\0';
    if (path[0] == L'\0' || path[1] != L':') return L'\0';
    wchar_t letter = path[0];
    if (letter >= L'a' && letter <= L'z') letter = (wchar_t)(letter - L'a' + L'A');
    if (letter < L'A' || letter > L'Z') return L'\0';
    return letter;
}

void FillBootDrive(HardwareSpecs& s) {
    s.bootDriveFs        = QueryDriveFilesystem(L'C');
    s.bootDriveMediaType = ClassifyDriveMedia(L'C');
}

void FillGameDrive(HardwareSpecs& s) {
    wchar_t letter = ResolveGameDriveLetter();
    if (letter == L'\0') return;
    s.gameDriveLetter    = (char)letter;
    s.gameDriveFs        = QueryDriveFilesystem(letter);
    s.gameDriveMediaType = ClassifyDriveMedia(letter);
}

using RtlGetVersionFn = LONG (WINAPI*)(RTL_OSVERSIONINFOW*);

void FillOs(HardwareSpecs& s) {
    HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
    if (nt) {
        auto fn = (RtlGetVersionFn)::GetProcAddress(nt, "RtlGetVersion");
        if (fn) {
            RTL_OSVERSIONINFOW v{};
            v.dwOSVersionInfoSize = sizeof(v);
            if (fn(&v) == 0) {
                s.osMajor = v.dwMajorVersion;
                s.osMinor = v.dwMinorVersion;
                s.osBuild = v.dwBuildNumber;
            }
        }
    }

    s.osProductName    = ReadRegStringHKLM(
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    s.osDisplayVersion = ReadRegStringHKLM(
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");

    if (s.osBuild >= 22000 && !s.osProductName.empty()) {
        auto pos = s.osProductName.find("Windows 10");
        if (pos != std::string::npos) {
            s.osProductName.replace(pos, 10, "Windows 11");
        }
    }

    uint32_t ubr = 0;
    if (ReadRegDwordHKLM(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                         L"UBR", ubr)) {
        s.osUbr = ubr;
    }
}

void FillDisplayMode(HardwareSpecs& s) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (::EnumDisplaySettingsExW(nullptr, ENUM_CURRENT_SETTINGS, &dm, 0)) {
        s.displayWidth     = dm.dmPelsWidth;
        s.displayHeight    = dm.dmPelsHeight;
        s.displayRefreshHz = dm.dmDisplayFrequency;
    }
}

enum REDSCOPE_DISPLAYCONFIG_DEVICE_INFO_TYPE_EXT : int {
    REDSCOPE_DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO = 9,
};

struct REDSCOPE_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
        struct {
            UINT32 advancedColorSupported    : 1;
            UINT32 advancedColorEnabled      : 1;
            UINT32 wideColorEnforced         : 1;
            UINT32 advancedColorForceDisabled: 1;
            UINT32 reserved                  : 28;
        };
        UINT32 value;
    };
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
    UINT32                       bitsPerColorChannel;
};

void FillDisplayHdr(HardwareSpecs& s) {
    UINT32 pathCount = 0, modeCount = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)
        != ERROR_SUCCESS) {
        s.displayHdrState = "unknown";
        return;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                             &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) {
        s.displayHdrState = "unknown";
        return;
    }
    bool resolved = false;
    for (UINT32 i = 0; i < pathCount; ++i) {
        REDSCOPE_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
        info.header.type =
            (DISPLAYCONFIG_DEVICE_INFO_TYPE)REDSCOPE_DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        info.header.size = sizeof(info);
        info.header.adapterId = paths[i].targetInfo.adapterId;
        info.header.id        = paths[i].targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS) {
            s.displayHdrState = info.advancedColorEnabled ? "on" : "off";
            resolved = true;
            break;
        }
    }
    if (!resolved) s.displayHdrState = "unknown";
}

uint64_t FileTimeToMs(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000ull;
}

void FillProcessContext(HardwareSpecs& s) {
    s.systemUptimeMs = ::GetTickCount64();

    FILETIME creation{}, exitT{}, kernelT{}, userT{}, nowSys{};
    if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exitT, &kernelT, &userT)) {
        ::GetSystemTimeAsFileTime(&nowSys);
        uint64_t nowMs = FileTimeToMs(nowSys);
        uint64_t crMs  = FileTimeToMs(creation);
        s.processCreationFileTimeMs = crMs;
        s.processUptimeMs = (nowMs > crMs) ? (nowMs - crMs) : 0;
    }

    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elev{};
        DWORD sz = 0;
        if (::GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz)) {
            s.processElevated = elev.TokenIsElevated != 0;
        }
        ::CloseHandle(token);
    }

    LPWSTR cmdW = ::GetCommandLineW();
    if (cmdW && *cmdW) {
        std::string narrow = WideToUtf8(cmdW);
        static const char kExe[] = "Cyberpunk2077.exe";
        std::string lower;
        lower.resize(narrow.size());
        for (size_t i = 0; i < narrow.size(); ++i) {
            char c = narrow[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            lower[i] = c;
        }
        auto pos = lower.find("cyberpunk2077.exe");
        if (pos != std::string::npos) {
            narrow = narrow.substr(pos);
            narrow.replace(0, sizeof(kExe) - 1, kExe);
            if (narrow.size() > sizeof(kExe) - 1 && narrow[sizeof(kExe) - 1] == '"') {
                narrow.erase(sizeof(kExe) - 1, 1);
            }
        }
        if (narrow.size() > 256) {
            narrow.resize(256);
        }
        s.commandLine = std::move(narrow);
    }
}

}

std::string TranslateNvidiaDriverVersion(std::string_view windowsVersion) noexcept {
    unsigned parts[4] = {0, 0, 0, 0};
    size_t idx = 0;
    size_t pos = 0;
    while (pos < windowsVersion.size() && idx < 4) {
        size_t dot = windowsVersion.find('.', pos);
        std::string_view seg = (dot == std::string_view::npos)
            ? windowsVersion.substr(pos)
            : windowsVersion.substr(pos, dot - pos);
        if (seg.empty()) return "";
        unsigned v = 0;
        auto res = std::from_chars(seg.data(), seg.data() + seg.size(), v);
        if (res.ec != std::errc{} || res.ptr != seg.data() + seg.size()) return "";
        parts[idx++] = v;
        if (dot == std::string_view::npos) {
            pos = windowsVersion.size();
            break;
        }
        pos = dot + 1;
    }
    if (idx != 4) return "";
    if (pos < windowsVersion.size()) return "";

    unsigned C = parts[2];
    unsigned D = parts[3];
    unsigned pubMajor = (C % 10) * 100 + D / 100;
    unsigned pubMinor = D % 100;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%02u", pubMajor, pubMinor);
    return std::string(buf);
}

void ParseSmbiosAndFillRam(const uint8_t* blob, uint32_t blobLen, HardwareSpecs& s) {
    if (!blob || blobLen < 8) return;
    uint32_t streamLen = (uint32_t)(blob[4] | (blob[5] << 8) |
                                    (blob[6] << 16) | (blob[7] << 24));
    if (streamLen == 0 || 8 + streamLen > blobLen) streamLen = blobLen - 8;
    const uint8_t* stream = blob + 8;
    const uint8_t* streamEnd = stream + streamLen;

    uint32_t fastestMHz = 0;
    uint32_t populated  = 0;
    std::string firstType;
    std::string firstManufacturer;
    std::string firstPartNumber;

    const uint8_t* cursor = stream;
    while (cursor + 4 <= streamEnd) {
        uint8_t type = cursor[0];
        uint8_t len  = cursor[1];
        if (len < 4 || cursor + len > streamEnd) break;

        const uint8_t* formatted = cursor;
        const uint8_t* strTable  = cursor + len;

        const uint8_t* p = strTable;
        while (p + 1 < streamEnd) {
            if (p[0] == 0 && p[1] == 0) { p += 2; break; }
            ++p;
        }
        if (p + 1 > streamEnd) break;

        if (type == 17 && len >= 0x15) {
            uint16_t size        = SmbiosReadU16(formatted + 0x0C);
            uint8_t  memType     = formatted[0x12];
            uint16_t rated       = SmbiosReadU16(formatted + 0x15);
            uint16_t configured  = (len >= 0x22) ? SmbiosReadU16(formatted + 0x20) : 0;

            if (size != 0 && size != 0xFFFF) {
                ++populated;
                uint16_t speed = configured != 0 ? configured : rated;
                if (speed > fastestMHz) fastestMHz = speed;
                if (firstType.empty()) {
                    const char* n = SmbiosMemoryTypeName(memType);
                    if (n && n[0]) firstType = n;
                }
                if (firstManufacturer.empty() && len >= 0x18) {
                    firstManufacturer = SmbiosString(formatted, len, p, formatted[0x17]);
                }
                if (firstPartNumber.empty() && len >= 0x1B) {
                    firstPartNumber = SmbiosString(formatted, len, p, formatted[0x1A]);
                    while (!firstPartNumber.empty() && firstPartNumber.back() == ' ') {
                        firstPartNumber.pop_back();
                    }
                }
            }
        }

        cursor = p;
        if (type == 127) break;
    }

    s.ramSpeedMHz     = fastestMHz;
    s.ramDimmCount    = populated;
    s.ramType         = std::move(firstType);
    s.ramManufacturer = std::move(firstManufacturer);
    s.ramPartNumber   = std::move(firstPartNumber);
}

std::string ClassifyDriveMedia(wchar_t driveLetter) {
    if (driveLetter >= L'a' && driveLetter <= L'z') {
        driveLetter = (wchar_t)(driveLetter - L'a' + L'A');
    }
    if (driveLetter < L'A' || driveLetter > L'Z') return "Unknown";

    wchar_t volPath[8];
    std::swprintf(volPath, 8, L"\\\\.\\%c:", driveLetter);

    HANDLE vol = ::CreateFileW(volPath, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (vol == INVALID_HANDLE_VALUE) return "Unknown";

    uint8_t extBuf[sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 4]{};
    auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(extBuf);
    DWORD ret = 0;
    BOOL ok = ::DeviceIoControl(vol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
        nullptr, 0, ext, sizeof(extBuf), &ret, nullptr);
    ::CloseHandle(vol);
    if (!ok || ext->NumberOfDiskExtents == 0) return "Unknown";

    DWORD diskNum = ext->Extents[0].DiskNumber;
    wchar_t physPath[64];
    std::swprintf(physPath, 64, L"\\\\.\\PhysicalDrive%lu", (unsigned long)diskNum);

    HANDLE disk = ::CreateFileW(physPath, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (disk == INVALID_HANDLE_VALUE) return "Unknown";

    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType  = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR desc{};
    ok = ::DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY,
        &q, sizeof(q), &desc, sizeof(desc), &ret, nullptr);
    ::CloseHandle(disk);
    if (!ok) return "Unknown";
    return desc.IncursSeekPenalty ? "HDD" : "SSD";
}

HardwareSpecs CaptureHardwareSpecs() {
    HardwareSpecs s;
    try { FillCpu(s);            } catch (...) {}
    try { FillGpu(s);            } catch (...) {}
    try { FillRam(s);            } catch (...) {}
    try { FillBootDrive(s);      } catch (...) {}
    try { FillGameDrive(s);      } catch (...) {}
    try { FillDisplayMode(s);    } catch (...) {}
    try { FillDisplayHdr(s);     } catch (...) {}
    try { FillOs(s);             } catch (...) {}
    try { FillProcessContext(s); } catch (...) {}
    return s;
}

}

namespace redscope::report {
namespace {

constexpr const char* kCpuIndent = "             ";
constexpr const char* kGpuIndent = "                 ";

void EmitCpuLine(PreallocatedBuffer& out, const snap::HardwareSpecs& hw) {
    if (hw.cpuName.empty() && hw.cpuLogicalCores == 0 && hw.cpuBaseMHz == 0) {
        out.Append("CPU:         (unavailable)\n");
        return;
    }
    const char* name = hw.cpuName.empty() ? "?" : hw.cpuName.c_str();
    out.Appendf("CPU:         %s\n", name);

    if (hw.cpuLogicalCores > 0 && hw.cpuBaseMHz > 0) {
        if (hw.cpuMaxTurboMHz > hw.cpuBaseMHz) {
            out.Appendf("%s%u logical / %u physical cores, %.1f GHz base (%.1f GHz max turbo)\n",
                        kCpuIndent,
                        hw.cpuLogicalCores, hw.cpuPhysicalCores,
                        (double)hw.cpuBaseMHz / 1000.0,
                        (double)hw.cpuMaxTurboMHz / 1000.0);
        } else {
            out.Appendf("%s%u logical / %u physical cores, %.1f GHz base\n",
                        kCpuIndent,
                        hw.cpuLogicalCores, hw.cpuPhysicalCores,
                        (double)hw.cpuBaseMHz / 1000.0);
        }
    } else if (hw.cpuLogicalCores > 0) {
        out.Appendf("%s%u logical / %u physical cores\n",
                    kCpuIndent, hw.cpuLogicalCores, hw.cpuPhysicalCores);
    } else if (hw.cpuBaseMHz > 0) {
        out.Appendf("%s%.1f GHz base\n",
                    kCpuIndent, (double)hw.cpuBaseMHz / 1000.0);
    }

    if (hw.cpuSupportsSse42 || hw.cpuSupportsAvx ||
        hw.cpuSupportsAvx2  || hw.cpuSupportsAvx512) {
        out.Appendf("%sAVX2: %s  AVX-512: %s  SSE4.2: %s\n",
                    kCpuIndent,
                    hw.cpuSupportsAvx2   ? "yes" : "no",
                    hw.cpuSupportsAvx512 ? "yes" : "no",
                    hw.cpuSupportsSse42  ? "yes" : "no");
    }
}

void EmitVramBytes(PreallocatedBuffer& out, uint64_t bytes, const char* suffix) {
    constexpr uint64_t kGB = 1024ull * 1024ull * 1024ull;
    constexpr uint64_t kMB = 1024ull * 1024ull;
    if (bytes >= kGB) {
        uint64_t gb = (bytes + kGB / 2) / kGB;
        out.Appendf("%llu GB %s", (unsigned long long)gb, suffix);
    } else if (bytes > 0) {
        uint64_t mb = (bytes + kMB / 2) / kMB;
        out.Appendf("%llu MB %s", (unsigned long long)mb, suffix);
    } else {
        out.Appendf("0 MB %s", suffix);
    }
}

bool IsNvidiaAdapter(const std::string& adapterName) noexcept {
    return IContainsAscii(adapterName.c_str(), "NVIDIA");
}

void EmitGpuBlock(PreallocatedBuffer& out, const snap::GpuAdapter& a,
                  const char* label) {
    const char* name = a.name.empty() ? "?" : a.name.c_str();
    if (!a.driverVersion.empty()) {
        if (IsNvidiaAdapter(a.name)) {
            std::string pub = redscope::snap::TranslateNvidiaDriverVersion(a.driverVersion);
            if (!pub.empty()) {
                out.Appendf("%s%s (driver %s = NVIDIA %s)\n",
                            label, name, a.driverVersion.c_str(), pub.c_str());
            } else {
                out.Appendf("%s%s (driver %s)\n", label, name, a.driverVersion.c_str());
            }
        } else {
            out.Appendf("%s%s (driver %s)\n", label, name, a.driverVersion.c_str());
        }
    } else {
        out.Appendf("%s%s\n", label, name);
    }
    if (a.vramDedicatedBytes > 0 || a.sharedSystemBytes > 0) {
        out.Append(kGpuIndent);
        EmitVramBytes(out, a.vramDedicatedBytes, "VRAM dedicated, ");
        EmitVramBytes(out, a.sharedSystemBytes, "shared\n");
    }
}

void EmitGpuLines(PreallocatedBuffer& out, const snap::HardwareSpecs& hw) {
    if (hw.gpus.empty()) {
        out.Append("GPU:         (unavailable)\n");
        return;
    }
    EmitGpuBlock(out, hw.gpus[0], "GPU (primary):   ");
    for (size_t i = 1; i < hw.gpus.size(); ++i) {
        char label[32];
        std::snprintf(label, sizeof(label), "GPU (adapter %zu): ", i);
        EmitGpuBlock(out, hw.gpus[i], label);
    }
}

void EmitRamLine(PreallocatedBuffer& out, const snap::HardwareSpecs& hw) {
    if (hw.ramTotalBytes == 0) {
        out.Append("RAM:         (unavailable)\n");
        return;
    }
    constexpr uint64_t kGB = 1024ull * 1024ull * 1024ull;
    uint64_t totalGb = (hw.ramTotalBytes + kGB / 2) / kGB;
    uint64_t availGb = (hw.ramAvailableBytes + kGB / 2) / kGB;
    out.Appendf("RAM:         %llu GB total, %llu GB available",
                (unsigned long long)totalGb,
                (unsigned long long)availGb);

    const bool hasType  = !hw.ramType.empty();
    const bool hasSpeed = hw.ramSpeedMHz > 0;
    const bool hasDimms = hw.ramDimmCount > 0;
    const bool hasMfr   = !hw.ramManufacturer.empty();
    if (hasType || hasSpeed || hasDimms || hasMfr) {
        out.Append("  (");
        bool wroteAny = false;
        if (hasType && hasSpeed) {
            out.Appendf("%s @ %u MHz", hw.ramType.c_str(), hw.ramSpeedMHz);
            wroteAny = true;
        } else if (hasType) {
            out.Appendf("%s", hw.ramType.c_str());
            wroteAny = true;
        } else if (hasSpeed) {
            out.Appendf("%u MHz", hw.ramSpeedMHz);
            wroteAny = true;
        }
        if (hasDimms) {
            if (wroteAny) out.Append(", ");
            uint64_t perDimm = hw.ramTotalBytes / hw.ramDimmCount;
            uint64_t perDimmGb = (perDimm + kGB / 2) / kGB;
            out.Appendf("%u\xC3\x97 %llu GB",
                        hw.ramDimmCount,
                        (unsigned long long)perDimmGb);
            wroteAny = true;
        }
        if (hasMfr) {
            if (wroteAny) out.Append(" ");
            out.Appendf("%s", hw.ramManufacturer.c_str());
        }
        out.Append(")");
    }
    out.Append("\n");
}

void EmitPageFileLine(PreallocatedBuffer& out, const snap::HardwareSpecs& hw) {
    if (hw.pageFileTotalBytes == 0) {
        out.Append("Page file:   (unavailable)\n");
        return;
    }
    constexpr uint64_t kGB = 1024ull * 1024ull * 1024ull;
    uint64_t totalGb = (hw.pageFileTotalBytes + kGB / 2) / kGB;
    uint64_t availGb = (hw.pageFileAvailBytes + kGB / 2) / kGB;
    out.Appendf("Page file:   %llu GB total, %llu GB available\n",
                (unsigned long long)totalGb,
                (unsigned long long)availGb);
}

void EmitDriveLine(PreallocatedBuffer& out, const char* label,
                   char letter,
                   const std::string& media,
                   const std::string& fsName) {
    if (media.empty() && fsName.empty()) {
        out.Appendf("%s  (unavailable)\n", label);
        return;
    }
    char letterOut = letter ? letter : '?';
    const char* mediaStr = media.empty() ? "?" : media.c_str();
    const char* fsStr    = fsName.empty() ? "?" : fsName.c_str();
    out.Appendf("%s  %c: %s (%s)\n", label, letterOut, mediaStr, fsStr);
}

void EmitDisplayLine(PreallocatedBuffer& out, const snap::HardwareSpecs& hw) {
    if (hw.displayWidth == 0 || hw.displayHeight == 0) {
        out.Append("Display:     (unavailable)\n");
        return;
    }
    const char* hdr = hw.displayHdrState.empty() ? "unknown" : hw.displayHdrState.c_str();
    if (hw.displayRefreshHz > 0) {
        out.Appendf("Display:     %u\xC3\x97%u @ %u Hz  HDR: %s\n",
                    hw.displayWidth, hw.displayHeight,
                    hw.displayRefreshHz, hdr);
    } else {
        out.Appendf("Display:     %u\xC3\x97%u  HDR: %s\n",
                    hw.displayWidth, hw.displayHeight, hdr);
    }
}

void EmitOsLine(PreallocatedBuffer& out, const snap::HardwareSpecs& hw) {
    if (hw.osProductName.empty() && hw.osBuild == 0) {
        out.Append("OS:          (unavailable)\n");
        return;
    }
    const char* product = hw.osProductName.empty() ? "Windows" : hw.osProductName.c_str();
    if (!hw.osDisplayVersion.empty() && hw.osBuild > 0) {
        out.Appendf("OS:          %s %s (build %u.%u)\n",
                    product, hw.osDisplayVersion.c_str(), hw.osBuild, hw.osUbr);
    } else if (hw.osBuild > 0) {
        out.Appendf("OS:          %s (build %u.%u)\n",
                    product, hw.osBuild, hw.osUbr);
    } else {
        out.Appendf("OS:          %s\n", product);
    }
}

void FormatSystemUptime(PreallocatedBuffer& out, uint64_t ms) {
    uint64_t totalSec = ms / 1000;
    uint64_t days  = totalSec / 86400;
    uint64_t hours = (totalSec % 86400) / 3600;
    uint64_t mins  = (totalSec % 3600) / 60;
    out.Appendf("%llud %lluh %llum",
                (unsigned long long)days,
                (unsigned long long)hours,
                (unsigned long long)mins);
}

void FormatProcessUptime(PreallocatedBuffer& out, uint64_t ms) {
    uint64_t totalSec = ms / 1000;
    uint64_t hours = totalSec / 3600;
    uint64_t mins  = (totalSec % 3600) / 60;
    uint64_t secs  = totalSec % 60;
    out.Appendf("%lluh %02llum %02llus",
                (unsigned long long)hours,
                (unsigned long long)mins,
                (unsigned long long)secs);
}

void EmitUptimeLine(PreallocatedBuffer& out, const snap::HardwareSpecs& hw) {
    const bool live = hw.processCreationFileTimeMs != 0;
    uint64_t sysMs  = live ? ::GetTickCount64() : hw.systemUptimeMs;

    uint64_t procMs = 0;
    if (live) {
        FILETIME nowSys{};
        ::GetSystemTimeAsFileTime(&nowSys);
        ULARGE_INTEGER u;
        u.LowPart  = nowSys.dwLowDateTime;
        u.HighPart = nowSys.dwHighDateTime;
        uint64_t nowMs = u.QuadPart / 10000ull;
        if (nowMs > hw.processCreationFileTimeMs) {
            procMs = nowMs - hw.processCreationFileTimeMs;
        }
    } else {
        procMs = hw.processUptimeMs;
    }

    const bool hasSys  = sysMs > 0;
    const bool hasProc = procMs > 0;
    if (!hasSys && !hasProc) {
        out.Append("Uptime:      (unavailable)\n");
        return;
    }
    out.Append("Uptime:      ");
    if (hasSys) {
        out.Append("system ");
        FormatSystemUptime(out, sysMs);
    }
    if (hasSys && hasProc) out.Append(", ");
    if (hasProc) {
        out.Append("process ");
        FormatProcessUptime(out, procMs);
    }
    out.Append("\n");
}

void EmitProcessLine(PreallocatedBuffer& out, const snap::HardwareSpecs& hw) {
    if (hw.commandLine.empty() && !hw.processElevated) {
        out.Append("Process:     elevated: no\n");
        return;
    }
    if (hw.commandLine.empty()) {
        out.Appendf("Process:     elevated: %s\n",
                    hw.processElevated ? "yes" : "no");
        return;
    }
    out.Appendf("Process:     elevated: %s  cmdline: %s\n",
                hw.processElevated ? "yes" : "no",
                hw.commandLine.c_str());
}

}

void EmitSystem(PreallocatedBuffer& out) {
    out.Append("--- System --------------------------------------------------------------------\n");
    const Snapshot* s = redscope::snap::Current();
    if (!s) {
        out.Append("(snapshot not yet available)\n\n");
        return;
    }
    const auto& hw = s->hardware;
    EmitCpuLine(out, hw);
    EmitGpuLines(out, hw);
    EmitRamLine(out, hw);
    EmitPageFileLine(out, hw);
    EmitDriveLine(out, "Boot drive:", 'C', hw.bootDriveMediaType, hw.bootDriveFs);
    EmitDriveLine(out, "Game drive:", hw.gameDriveLetter,
                  hw.gameDriveMediaType, hw.gameDriveFs);
    EmitDisplayLine(out, hw);
    EmitOsLine(out, hw);
    EmitUptimeLine(out, hw);
    EmitProcessLine(out, hw);

    const auto& g = s->gpu;
    char hints[512] = {};
    size_t pos = 0;
    for (size_t i = 0; i < snap::kGpuHintCount; ++i) {
        if (!g.hintLabels[i][0] || !g.hintPaths[i][0]) continue;
        int n = std::snprintf(hints + pos, sizeof(hints) - pos,
                              "%s%s=%s%s",
                              pos == 0 ? "" : ", ",
                              g.hintLabels[i],
                              g.hintPaths[i],
                              g.hintExists[i] ? "" : " (missing)");
        if (n < 0) break;
        pos += (size_t)n;
        if (pos >= sizeof(hints) - 1) break;
    }
    if (pos > 0) {
        out.Appendf("Crash dumps: %s\n", hints);
    }
    out.Append("\n");
}

}
