#include "LogTailer.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace redscope::logs {
namespace {

HANDLE OpenForRead(const std::filesystem::path& path) {
    return ::CreateFileW(path.c_str(),
                         GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL,
                         nullptr);
}

bool ReadTailBytes(const std::filesystem::path& path, size_t maxBytes, std::string& out) {
    HANDLE h = OpenForRead(path);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) { ::CloseHandle(h); return false; }

    uint64_t fileSize = (uint64_t)size.QuadPart;
    uint64_t toRead = fileSize > maxBytes ? maxBytes : fileSize;
    uint64_t offset = fileSize - toRead;

    if (offset > 0) {
        LARGE_INTEGER pos{};
        pos.QuadPart = (LONGLONG)offset;
        if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
            ::CloseHandle(h);
            return false;
        }
    }

    out.resize((size_t)toRead);
    DWORD totalRead = 0;
    while (totalRead < toRead) {
        DWORD chunk = 0;
        DWORD want = (DWORD)((toRead - totalRead) > 0x10000000u ? 0x10000000u : (toRead - totalRead));
        if (!::ReadFile(h, out.data() + totalRead, want, &chunk, nullptr) || chunk == 0) {
            ::CloseHandle(h);
            out.resize(totalRead);
            return totalRead > 0 || toRead == 0;
        }
        totalRead += chunk;
    }
    ::CloseHandle(h);

    if (offset > 0) {
        size_t nl = out.find('\n');
        if (nl != std::string::npos) out.erase(0, nl + 1);
    }
    return true;
}

void SplitLines(const std::string& text, std::vector<std::string>& out) {
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            size_t end = i;
            if (end > start && text[end - 1] == '\r') --end;
            out.emplace_back(text, start, end - start);
            start = i + 1;
        }
    }
    if (start < text.size()) {
        size_t end = text.size();
        if (end > start && text[end - 1] == '\r') --end;
        out.emplace_back(text, start, end - start);
    }
}

bool ParseLeadingTimestamp(const std::string& line, std::time_t& epochOut) {
    const char* p = line.c_str();
    if (*p == '[') ++p;

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    int n = std::sscanf(p, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s);
    if (n != 6) return false;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) return false;

    std::tm tmv{};
    tmv.tm_year = y - 1900;
    tmv.tm_mon  = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min  = mi;
    tmv.tm_sec  = s;
    tmv.tm_isdst = -1;

    std::time_t t = std::mktime(&tmv);
    if (t == (std::time_t)-1) return false;
    epochOut = t;
    return true;
}

bool GlobMatch(const char* pattern, const char* name) {
    const char* pStar = nullptr;
    const char* nMark = nullptr;

    while (*name) {
        if (*pattern == '*') {
            pStar = pattern++;
            nMark = name;
        } else if (*pattern == '?' ||
                   (unsigned char)::tolower((unsigned char)*pattern) ==
                   (unsigned char)::tolower((unsigned char)*name)) {
            ++pattern;
            ++name;
        } else if (pStar) {
            pattern = pStar + 1;
            name = ++nMark;
        } else {
            return false;
        }
    }
    while (*pattern == '*') ++pattern;
    return *pattern == '\0';
}

}

std::vector<std::string> Tail(const std::filesystem::path& path, size_t maxLines) {
    std::vector<std::string> result;
    if (maxLines == 0) return result;

    constexpr size_t kMaxReadBytes = 1024 * 1024;
    std::string text;
    if (!ReadTailBytes(path, kMaxReadBytes, text)) return result;

    std::vector<std::string> ring;
    ring.reserve(maxLines);
    size_t head = 0;
    bool   wrapped = false;
    auto push = [&](size_t s, size_t e) {
        if (ring.size() < maxLines) {
            ring.emplace_back(text, s, e - s);
        } else {
            ring[head].assign(text, s, e - s);
            head = (head + 1) % maxLines;
            wrapped = true;
        }
    };

    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            size_t end = i;
            if (end > start && text[end - 1] == '\r') --end;
            push(start, end);
            start = i + 1;
        }
    }
    if (start < text.size()) {
        size_t end = text.size();
        if (end > start && text[end - 1] == '\r') --end;
        push(start, end);
    }

    result.reserve(ring.size());
    if (!wrapped) {
        for (auto& s : ring) result.push_back(std::move(s));
    } else {
        for (size_t k = 0; k < maxLines; ++k) {
            result.push_back(std::move(ring[(head + k) % maxLines]));
        }
    }
    return result;
}

std::vector<std::string> WindowAround(const std::filesystem::path& path,
                                      std::chrono::system_clock::time_point crashTime,
                                      int windowSec,
                                      size_t maxLines) {
    std::vector<std::string> result;
    if (maxLines == 0 || windowSec < 0) return result;

    constexpr size_t kMaxReadBytes = 1024 * 1024;
    std::string text;
    if (!ReadTailBytes(path, kMaxReadBytes, text)) return result;

    std::vector<std::string> lines;
    lines.reserve(64);
    SplitLines(text, lines);

    auto crashEpoch = std::chrono::system_clock::to_time_t(crashTime);
    std::time_t lo = crashEpoch - windowSec;
    std::time_t hi = crashEpoch + windowSec;

    std::vector<std::string> kept;
    kept.reserve(lines.size());
    bool currentInWindow = false;
    bool seenAnyTimestamp = false;
    for (const auto& ln : lines) {
        std::time_t ts = 0;
        if (ParseLeadingTimestamp(ln, ts)) {
            currentInWindow = (ts >= lo && ts <= hi);
            seenAnyTimestamp = true;
            if (currentInWindow) kept.push_back(ln);
        } else {
            if (seenAnyTimestamp && currentInWindow) kept.push_back(ln);
        }
    }

    if (kept.size() > maxLines) {
        result.assign(kept.end() - (std::ptrdiff_t)maxLines, kept.end());
    } else {
        result = std::move(kept);
    }
    return result;
}

std::filesystem::path FindNewestMatching(const std::filesystem::path& dir,
                                         const std::string& glob) {
    std::filesystem::path newest;
    FILETIME newestTime{};
    bool haveAny = false;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return newest;

    std::wstring search = (dir / L"*").wstring();
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return newest;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        char nameUtf8[MAX_PATH * 4];
        int nb = ::WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                       nameUtf8, (int)sizeof(nameUtf8),
                                       nullptr, nullptr);
        if (nb == 0) continue;

        if (!GlobMatch(glob.c_str(), nameUtf8)) continue;

        if (!haveAny || ::CompareFileTime(&fd.ftLastWriteTime, &newestTime) > 0) {
            newest = dir / fd.cFileName;
            newestTime = fd.ftLastWriteTime;
            haveAny = true;
        }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);

    return newest;
}

}
