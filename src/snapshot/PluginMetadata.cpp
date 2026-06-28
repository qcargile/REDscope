#include "PluginMetadata.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace redscope::snap {
namespace {

namespace fs = std::filesystem;

std::string LowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out.push_back(c);
    }
    return out;
}

std::string_view Basename(std::string_view path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

std::string_view Trim(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
    return s.substr(b, e - b);
}

fs::path PickNewestRed4extLog(const fs::path& logsDir) {
    std::error_code ec;
    if (!fs::exists(logsDir, ec) || !fs::is_directory(logsDir, ec)) return {};

    fs::path newest;
    std::string newestName;
    for (const auto& entry : fs::directory_iterator(logsDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name.size() < 11) continue;
        if (name.compare(0, 8, "red4ext-") != 0) continue;
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".log") != 0) continue;
        if (name > newestName) {
            newestName = std::move(name);
            newest = entry.path();
        }
    }
    return newest;
}

bool SlurpFile(const fs::path& path, std::string& out, size_t maxBytes = 1024 * 1024) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streampos end = f.tellg();
    f.seekg(0, std::ios::beg);
    if (end <= 0) { out.clear(); return true; }
    size_t toRead = (size_t)end;
    if (toRead > maxBytes) toRead = maxBytes;
    out.resize(toRead);
    f.read(out.data(), (std::streamsize)out.size());
    out.resize((size_t)f.gcount());
    return true;
}

std::string_view StripRed4extInfoPrefix(std::string_view line) {
    static constexpr std::string_view kInfix = "[RED4ext] [info] ";
    size_t pos = line.find(kInfix);
    if (pos == std::string_view::npos) return {};
    return line.substr(pos + kInfix.size());
}

std::string_view StripRed4extErrorPrefix(std::string_view line) {
    static constexpr std::string_view kInfix = "[RED4ext] [error] ";
    size_t pos = line.find(kInfix);
    if (pos == std::string_view::npos) return {};
    return line.substr(pos + kInfix.size());
}

std::string_view ParseLoadingPluginPath(std::string_view body) {
    static constexpr std::string_view kPrefix = "Loading plugin from '";
    if (body.compare(0, kPrefix.size(), kPrefix) != 0) return {};
    std::string_view rest = body.substr(kPrefix.size());
    size_t end = rest.find('\'');
    if (end == std::string_view::npos) return {};
    return rest.substr(0, end);
}

bool ParseHasBeenLoaded(std::string_view body,
                        std::string_view& outName,
                        std::string_view& outVer,
                        std::string_view& outAuthors) {
    static constexpr std::string_view kSuffix = ") has been loaded";
    if (body.size() < kSuffix.size()) return false;
    if (body.compare(body.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) return false;
    std::string_view inner = body.substr(0, body.size() - kSuffix.size());

    static constexpr std::string_view kVersionTag = " (version: ";
    size_t vpos = inner.find(kVersionTag);
    if (vpos == std::string_view::npos) return false;
    outName = Trim(inner.substr(0, vpos));
    if (outName.empty()) return false;

    std::string_view tail = inner.substr(vpos + kVersionTag.size());
    static constexpr std::string_view kAuthorTag = ", author(s): ";
    size_t apos = tail.find(kAuthorTag);
    if (apos == std::string_view::npos) return false;
    outVer     = Trim(tail.substr(0, apos));
    outAuthors = Trim(tail.substr(apos + kAuthorTag.size()));
    return true;
}

bool IsFailedLoadLine(std::string_view body) {
    static constexpr std::string_view kPrefix = "Failed to load plugin";
    return body.compare(0, kPrefix.size(), kPrefix) == 0;
}

std::string_view ExtractTimestamp(std::string_view line) {
    if (line.size() < 21 || line[0] != '[') return {};
    size_t close = line.find(']');
    if (close == std::string_view::npos || close < 20) return {};
    return line.substr(1, 19);
}

template <typename Fn>
void ForEachLine(std::string_view text, Fn&& cb) {
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(i, end - i);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        cb(line);
        i = end + 1;
    }
}

void ParseRed4extLog(std::string_view text, PluginMetadata& out) {
    if (text.size() >= 3 &&
        (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text.remove_prefix(3);
    }

    std::string pendingDll;
    bool firstLine = true;

    ForEachLine(text, [&](std::string_view line) {
        if (firstLine) {
            firstLine = false;
            std::string_view ts = ExtractTimestamp(line);
            if (!ts.empty()) out.logTimestamp.assign(ts.data(), ts.size());
        }

        std::string_view body = StripRed4extInfoPrefix(line);
        if (body.empty()) {
            std::string_view errBody = StripRed4extErrorPrefix(line);
            if (!errBody.empty() && IsFailedLoadLine(errBody)) {
                ++out.failedLoadCount;
                pendingDll.clear();
            }
            return;
        }

        std::string_view dllPath = ParseLoadingPluginPath(body);
        if (!dllPath.empty()) {
            pendingDll.assign(Basename(dllPath));
            return;
        }

        std::string_view name, ver, authors;
        if (ParseHasBeenLoaded(body, name, ver, authors)) {
            PluginInfo info;
            info.name.assign(name);
            info.version.assign(ver);
            info.authors.assign(authors);
            info.dllBasename = LowerCopy(pendingDll);
            std::string key = info.dllBasename.empty()
                                ? LowerCopy(info.name)
                                : info.dllBasename;
            out.byDll.emplace(std::move(key), std::move(info));
            pendingDll.clear();
            return;
        }

        if (IsFailedLoadLine(body)) {
            ++out.failedLoadCount;
            pendingDll.clear();
        }
    });
}

std::string ExtractCetVersion(std::string_view text) {
    static constexpr std::string_view kTag = "] CET version ";
    std::string result;
    uint32_t linesScanned = 0;
    ForEachLine(text, [&](std::string_view line) {
        if (!result.empty() || linesScanned >= 20) return;
        ++linesScanned;
        size_t pos = line.find(kTag);
        if (pos == std::string_view::npos) return;
        std::string_view rest = line.substr(pos + kTag.size());
        size_t end = 0;
        while (end < rest.size() && rest[end] != ' ' && rest[end] != '\t'
                                 && rest[end] != '[' && rest[end] != '\r') {
            ++end;
        }
        if (end == 0) return;
        std::string_view tok = rest.substr(0, end);
        if (!tok.empty() && (tok.front() == 'v' || tok.front() == 'V')) {
            tok.remove_prefix(1);
        }
        result.assign(tok);
    });
    return result;
}

void ParseCetLog(const fs::path& cetLogPath, PluginMetadata& out) {
    std::error_code ec;
    if (!fs::exists(cetLogPath, ec)) return;
    std::string text;
    if (!SlurpFile(cetLogPath, text)) return;
    out.cetVersion = ExtractCetVersion(text);
}

}

PluginMetadata EnumeratePluginMetadata(const fs::path& gameRoot) {
    PluginMetadata out;

    fs::path logsDir = gameRoot / "red4ext" / "logs";
    fs::path newest = PickNewestRed4extLog(logsDir);
    if (newest.empty()) {
        out.source = PluginMetadata::Source::NoLogFound;
    } else {
        std::string text;
        if (!SlurpFile(newest, text)) {
            out.source = PluginMetadata::Source::ParseError;
        } else {
            ParseRed4extLog(text, out);
            out.source = PluginMetadata::Source::Red4extLog;
        }
    }

    ParseCetLog(gameRoot / "bin" / "x64" / "plugins" / "cyber_engine_tweaks"
                    / "cyber_engine_tweaks.log",
                out);

    return out;
}

}
