#include "GpuState.h"
#include "../util/FixedStr.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

namespace redscope::snap {

namespace {

std::string LocalAppDataDir() {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) return {};
    std::filesystem::path p(buf);
    return p.string();
}

std::string UserProfileDir() {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, buf))) return {};
    std::filesystem::path p(buf);
    return p.string();
}

void RedactUserProfile(std::string& s) {
    static const std::string profile = UserProfileDir();
    if (profile.empty() || s.size() < profile.size()) return;
    if (::_strnicmp(s.c_str(), profile.c_str(), profile.size()) == 0) {
        s.replace(0, profile.size(), "%USERPROFILE%");
    }
}

bool DirExists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec);
}

void Set(GpuState& out, size_t i, const char* label, const std::string& path) {
    if (i >= kGpuHintCount) return;
    CopyFixed(out.hintLabels[i], sizeof(out.hintLabels[i]), label);
    std::string display = path;
    RedactUserProfile(display);
    CopyFixed(out.hintPaths [i], sizeof(out.hintPaths [i]), display.c_str());
    out.hintExists[i] = DirExists(path);
}

}

void CaptureInto(GpuState& out, const std::filesystem::path& gameRoot) noexcept {
    out = GpuState{};

    const std::string localAppData = LocalAppDataDir();

    if (!localAppData.empty()) {
        Set(out, 0, "NVIDIA Aftermath", localAppData + "\\NVIDIA\\DXCache");
    }

    if (!localAppData.empty()) {
        Set(out, 1, "Windows CrashDumps", localAppData + "\\CrashDumps");
    }

    if (!gameRoot.empty()) {
        Set(out, 2, "Game bin/x64", (gameRoot / "bin" / "x64").string());
    }

    if (!localAppData.empty()) {
        Set(out, 3, "CDPR user cache",
            localAppData + "\\CD Projekt Red\\Cyberpunk 2077");
    }
}

}
