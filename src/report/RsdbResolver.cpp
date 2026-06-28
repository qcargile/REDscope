#include "RsdbResolver.h"
#include <windows.h>
#include <cstring>

namespace redscope::report {

namespace {

uint32_t ReadU32LE(const unsigned char* p) noexcept {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64BE(const unsigned char* p) noexcept {
    return (static_cast<uint64_t>(p[0]) << 56)
         | (static_cast<uint64_t>(p[1]) << 48)
         | (static_cast<uint64_t>(p[2]) << 40)
         | (static_cast<uint64_t>(p[3]) << 32)
         | (static_cast<uint64_t>(p[4]) << 24)
         | (static_cast<uint64_t>(p[5]) << 16)
         | (static_cast<uint64_t>(p[6]) << 8)
         |  static_cast<uint64_t>(p[7]);
}

bool ReadAt(std::FILE* f, long long offset, void* dst, size_t n) noexcept {
    if (_fseeki64(f, offset, SEEK_SET) != 0) return false;
    return std::fread(dst, 1, n, f) == n;
}

}

RsdbDict::~RsdbDict() {
    if (m_file) { std::fclose(m_file); m_file = nullptr; }
}

bool RsdbDict::Open(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) return false;
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path.wstring().c_str(), L"rb") != 0 || !f) return false;
    unsigned char header[16] = {};
    if (!ReadAt(f, 0, header, sizeof(header)) || std::memcmp(header, "RSD1", 4) != 0) {
        std::fclose(f);
        return false;
    }
    m_count       = ReadU32LE(header + 4);
    m_indexOffset = ReadU32LE(header + 8);
    m_pathsOffset = ReadU32LE(header + 12);
    if (m_count == 0) { std::fclose(f); return false; }
    m_file = f;
    return true;
}

bool RsdbDict::Lookup(uint64_t hash, std::string& out) const noexcept {
    if (!m_file || m_count == 0) return false;
    long long lo = 0;
    long long hi = static_cast<long long>(m_count) - 1;
    while (lo <= hi) {
        long long mid = lo + (hi - lo) / 2;
        unsigned char entry[16] = {};
        if (!ReadAt(m_file, static_cast<long long>(m_indexOffset) + mid * 16, entry, sizeof(entry))) {
            return false;
        }
        uint64_t key = ReadU64BE(entry);
        if (key == hash) {
            uint32_t pathOff = ReadU32LE(entry + 8);
            uint32_t pathLen = ReadU32LE(entry + 12);
            if (pathLen == 0 || pathLen > 4096) return false;
            out.resize(pathLen);
            if (!ReadAt(m_file, static_cast<long long>(m_pathsOffset) + pathOff, &out[0], pathLen)) {
                return false;
            }
            return true;
        }
        if (key < hash) { lo = mid + 1; } else { hi = mid - 1; }
    }
    return false;
}

std::filesystem::path ResolveRsdbPath() noexcept {
    HMODULE exe = ::GetModuleHandleW(L"Cyberpunk2077.exe");
    if (!exe) return {};
    wchar_t modulePath[MAX_PATH] = {};
    if (::GetModuleFileNameW(exe, modulePath, MAX_PATH) == 0) return {};
    std::filesystem::path root =
        std::filesystem::path(modulePath).parent_path().parent_path().parent_path();
    return root / L"bin" / L"x64" / L"plugins" / L"cyber_engine_tweaks"
                / L"mods" / L"REDscope" / L"dictionary" / L"redscope-paths.rsdb";
}

}
