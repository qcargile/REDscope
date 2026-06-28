#include "WrapChain.h"
#include "../util/FixedStr.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace redscope::snap {

namespace {

bool IsIdentStart(unsigned char c) noexcept {
    return std::isalpha(c) || c == '_';
}

bool IsIdentChar(unsigned char c) noexcept {
    return std::isalnum(c) || c == '_';
}

bool IsWhitespace(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void Trim(std::string& s) {
    size_t i = 0;
    while (i < s.size() && IsWhitespace((unsigned char)s[i])) ++i;
    size_t j = s.size();
    while (j > i && IsWhitespace((unsigned char)s[j - 1])) --j;
    s.erase(j);
    s.erase(0, i);
}

std::string NormaliseForMatch(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c)) out.push_back((char)std::tolower(c));
    }
    return out;
}

std::string AttributeMod(const std::filesystem::path& fullPath,
                         const std::filesystem::path& relToScripts,
                         const std::vector<std::string>& modNames,
                         const std::unordered_map<std::string, std::string>& normToName) {
    for (const auto& comp : fullPath) {
        std::string s = comp.string();
        if (s.empty()) continue;
        std::string norm = NormaliseForMatch(s);
        if (norm.empty()) continue;
        auto it = normToName.find(norm);
        if (it != normToName.end()) return it->second;
    }

    for (const auto& comp : relToScripts) {
        std::string s = comp.string();
        if (s.empty() || s == "..") continue;
        if (comp == relToScripts.filename()) {
            auto stem = relToScripts.stem().string();
            return stem.empty() ? s : stem;
        }
        return s;
    }

    auto stem = relToScripts.stem().string();
    if (!stem.empty()) return stem;
    return "";
}

WrapChainEntry* FindOrCreateChain(WrapChainTable& t, const char* key) {
    for (auto& c : t.chains) {
        if (std::strcmp(c.methodKey, key) == 0) return &c;
    }
    if (t.chains.size() >= kMaxWrapChains) {
        ++t.chainsDropped;
        return nullptr;
    }
    t.chains.emplace_back();
    WrapChainEntry& e = t.chains.back();
    CopyFixed(e.methodKey, kWrapMethodKeyCap, key);
    return &e;
}

void AppendLayer(WrapChainEntry& e,
                 const std::string& modName,
                 const std::string& relFile,
                 uint32_t lineNumber) {
    if (e.layerCount >= kMaxWrapLayers) {
        ++e.extraLayers;
        return;
    }
    WrapLayer& l = e.layers[e.layerCount++];
    CopyFixed(l.modName, kWrapModNameCap, modName.c_str());
    CopyFixed(l.relFile, kWrapRelFileCap, relFile.c_str());
    l.lineNumber = lineNumber;
}

size_t FirstNonSpace(const std::string& line) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (!IsWhitespace((unsigned char)line[i])) return i;
    }
    return std::string::npos;
}

std::string StripComments(const std::string& line, bool& inBlockComment) {
    std::string out = line;
    size_t i = 0;
    while (i < out.size()) {
        if (inBlockComment) {
            if (i + 1 < out.size() && out[i] == '*' && out[i + 1] == '/') {
                out[i]     = ' ';
                out[i + 1] = ' ';
                inBlockComment = false;
                i += 2;
            } else {
                out[i] = ' ';
                ++i;
            }
            continue;
        }
        if (i + 1 < out.size() && out[i] == '/' && out[i + 1] == '/') {
            for (size_t j = i; j < out.size(); ++j) out[j] = ' ';
            break;
        }
        if (i + 1 < out.size() && out[i] == '/' && out[i + 1] == '*') {
            inBlockComment = true;
            out[i]     = ' ';
            out[i + 1] = ' ';
            i += 2;
            continue;
        }
        ++i;
    }
    return out;
}

struct PendingLayer {
    std::string methodKey;
    std::string modName;
    std::string relFile;
    uint32_t    lineNumber;
};

void ScanFile(const std::filesystem::path& file,
              const std::filesystem::path& scriptsRoot,
              const std::vector<std::string>& modNames,
              const std::unordered_map<std::string, std::string>& normToName,
              std::vector<PendingLayer>& out) {
    std::ifstream in(file);
    if (!in) return;

    auto rel = file.lexically_relative(scriptsRoot);
    if (rel.empty()) rel = file.filename();
    std::string relStr = rel.generic_string();
    std::string modName = AttributeMod(file, rel, modNames, normToName);

    std::string rawLine;
    uint32_t    lineNumber   = 0;
    bool        inBlockComment = false;

    std::string pendingClass;
    uint32_t    pendingLine  = 0;

    std::string cls, meth;
    while (std::getline(in, rawLine)) {
        ++lineNumber;
        std::string line = StripComments(rawLine, inBlockComment);

        if (FirstNonSpace(line) == std::string::npos) {
            continue;
        }

        if (pendingClass.empty()) {
            if (ParseWrapAnnotation(line.c_str(), cls)) {
                pendingClass = cls;
                pendingLine  = lineNumber;
            }
            continue;
        }

        if (ParseFuncDeclaration(line.c_str(), meth)) {
            char key[kWrapMethodKeyCap];
            BuildMethodKey(pendingClass.c_str(), meth.c_str(), key, sizeof(key));
            out.push_back({ key, modName, relStr, pendingLine });
            pendingClass.clear();
        } else {
            if (ParseWrapAnnotation(line.c_str(), cls)) {
                pendingClass = cls;
                pendingLine  = lineNumber;
            } else {
                pendingClass.clear();
            }
        }
    }
}

}

bool ParseWrapAnnotation(const char* line, std::string& outClass) {
    if (!line) return false;
    const char* needle = std::strstr(line, "@wrapMethod");
    if (!needle) return false;

    const char* after = needle + std::strlen("@wrapMethod");
    if (IsIdentChar((unsigned char)*after)) return false;

    const char* p = after;
    while (*p && IsWhitespace((unsigned char)*p)) ++p;
    if (*p != '(') return false;
    ++p;

    std::string inner;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '(') ++depth;
        else if (*p == ')') { --depth; if (depth == 0) break; }
        inner.push_back(*p);
        ++p;
    }
    if (depth != 0) return false;
    Trim(inner);

    if (inner.empty()) return false;
    for (char c : inner) if (!IsIdentChar((unsigned char)c)) return false;
    outClass = inner;
    return true;
}

bool ParseFuncDeclaration(const char* line, std::string& outMethod) {
    if (!line) return false;

    const char* scan = line;
    while ((scan = std::strstr(scan, "func")) != nullptr) {
        const char* before = (scan == line) ? nullptr : (scan - 1);
        const char* after  = scan + 4;
        bool leftBoundary  = !before || !IsIdentChar((unsigned char)*before);
        bool rightBoundary = !*after || IsWhitespace((unsigned char)*after);
        if (leftBoundary && rightBoundary) break;
        ++scan;
    }
    if (!scan) return false;

    const char* p = scan + 4;
    while (*p && IsWhitespace((unsigned char)*p)) ++p;

    if (!*p || !IsIdentStart((unsigned char)*p)) return false;

    std::string name;
    while (*p && IsIdentChar((unsigned char)*p)) {
        name.push_back(*p);
        ++p;
    }
    if (name.empty()) return false;

    while (*p && IsWhitespace((unsigned char)*p)) ++p;
    if (*p != '(') return false;

    outMethod = name;
    return true;
}

void BuildMethodKey(const char* cls, const char* meth,
                    char* out, size_t cap) {
    if (cap == 0) return;
    size_t i = 0;
    if (cls) {
        for (; cls[i] && i + 1 < cap; ++i) out[i] = cls[i];
    }
    if (i + 1 < cap) out[i++] = '.';
    size_t j = 0;
    if (meth) {
        for (; meth[j] && i + 1 < cap; ++j, ++i) out[i] = meth[j];
    }
    out[i] = '\0';
}

void NormalizeWrapMethodKey(const char* fullName, char* key, size_t cap) {
    if (cap == 0) return;
    size_t j = 0;
    if (fullName) {
        for (; fullName[j] && fullName[j] != ';' && j + 1 < cap; ++j) key[j] = fullName[j];
    }
    key[j] = '\0';
    while (j > 0 && (key[j - 1] == ' ' || key[j - 1] == '\t')) key[--j] = '\0';
}

const WrapChainEntry* FindChain(const WrapChainTable& t,
                                const char* classDotMethod) noexcept {
    if (!classDotMethod) return nullptr;
    for (const auto& c : t.chains) {
        if (std::strcmp(c.methodKey, classDotMethod) == 0) return &c;
    }
    return nullptr;
}

WrapChainTable CaptureWrapChains(const std::filesystem::path& gameRoot,
                                 const ModInventory& inventory) {
    WrapChainTable t{};

    if (gameRoot.empty()) return t;
    std::filesystem::path scriptsRoot = gameRoot / "r6" / "scripts";
    std::error_code ec;
    if (!std::filesystem::exists(scriptsRoot, ec)) return t;

    t.scanPerformed = true;

    std::vector<std::string> modNames;
    std::unordered_map<std::string, std::string> normToName;
    modNames.reserve(inventory.mods.size());
    normToName.reserve(inventory.mods.size());
    for (const auto& m : inventory.mods) {
        modNames.push_back(m.name);
        std::string norm = NormaliseForMatch(m.name);
        if (!norm.empty()) normToName.emplace(std::move(norm), m.name);
    }

    std::vector<PendingLayer> pending;
    pending.reserve(1024);
    t.chains.reserve(kMaxWrapChains);

    for (auto it = std::filesystem::recursive_directory_iterator(
             scriptsRoot, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{};
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const auto& p = it->path();
        if (p.extension() != ".reds") continue;

        ++t.filesScanned;
        ScanFile(p, scriptsRoot, modNames, normToName, pending);
    }

    std::sort(pending.begin(), pending.end(),
              [](const PendingLayer& a, const PendingLayer& b) {
                  if (a.relFile != b.relFile) return a.relFile < b.relFile;
                  return a.lineNumber < b.lineNumber;
              });

    std::unordered_set<std::string> modsWithWraps;
    for (const auto& pl : pending) {
        auto* chain = FindOrCreateChain(t, pl.methodKey.c_str());
        if (!chain) continue;
        AppendLayer(*chain, pl.modName, pl.relFile, pl.lineNumber);
        ++t.totalLayers;
        if (!pl.modName.empty()) modsWithWraps.insert(pl.modName);
    }
    t.modsWithWraps = (uint32_t)modsWithWraps.size();

    return t;
}

}
