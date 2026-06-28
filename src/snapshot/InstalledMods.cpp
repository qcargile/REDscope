#include "InstalledMods.h"
#include "Snapshot.h"
#include "SnapshotWorker.h"
#include "../report/Sections.h"
#include "../util/PreallocatedBuffer.h"
#include <windows.h>
#include <simdjson.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace redscope::snap {
namespace {

namespace fs = std::filesystem;

void ClampToCap(ModInventory& inv) {
    if (inv.mods.size() > kMaxInstalledMods) {
        inv.truncated = (uint32_t)(inv.mods.size() - kMaxInstalledMods);
        inv.mods.resize(kMaxInstalledMods);
    }
}

std::string Trim(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
    return std::string(s.substr(b, e - b));
}

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

std::string ReadIniKey(const fs::path& iniPath,
                       std::string_view section,
                       std::string_view key) {
    std::ifstream f(iniPath, std::ios::binary);
    if (!f) return {};

    std::string line;
    bool inSection = false;
    while (std::getline(f, line)) {
        std::string t = Trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            std::string name = Trim(std::string_view(t).substr(1, t.size() - 2));
            inSection = IEquals(name, section);
            continue;
        }
        if (!inSection) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(std::string_view(t).substr(0, eq));
        if (!IEquals(k, key)) continue;
        std::string val = Trim(std::string_view(t).substr(eq + 1));

        if (val.size() > 11 &&
            val.compare(0, 10, "@ByteArray") == 0 &&
            val[10] == '(' &&
            val.back() == ')') {
            val = val.substr(11, val.size() - 12);
        }
        return val;
    }
    return {};
}

std::string ResolveMO2Profile(const fs::path& mo2Root) {
    std::string selected = ReadIniKey(mo2Root / "ModOrganizer.ini",
                                      "General", "selected_profile");
    if (!selected.empty()) return selected;

    fs::path profiles = mo2Root / "profiles";
    std::error_code ec;
    if (!fs::exists(profiles, ec) || !fs::is_directory(profiles, ec)) return {};
    std::string only;
    size_t count = 0;
    for (const auto& e : fs::directory_iterator(profiles, ec)) {
        if (!e.is_directory(ec)) continue;
        if (++count > 1) return {};
        only = e.path().filename().string();
    }
    return count == 1 ? only : std::string();
}

bool ExtEquals(const fs::path& p, std::string_view ext) {
    auto extStr = p.extension().string();
    if (extStr.size() != ext.size()) return false;
    for (size_t i = 0; i < ext.size(); ++i) {
        char a = extStr[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

std::string FirstFileRelative(const fs::path& modDir,
                              const fs::path& root,
                              std::string_view ext) {
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return {};
    for (fs::recursive_directory_iterator it(root, ec), end;
         !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (!ExtEquals(it->path(), ext)) continue;
        fs::path rel = it->path().lexically_relative(modDir);
        if (rel.empty()) return {};
        std::string s = rel.generic_string();
        return s;
    }
    return {};
}

void CollectArchivesInto(std::vector<std::string>& out, const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, ec), end;
         !ec && it != end && out.size() < kMaxArchivesPerMod;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (!ExtEquals(it->path(), ".archive")) continue;
        out.push_back(it->path().filename().string());
    }
}

ModType ClassifyMO2Mod(const fs::path& modDir) {
    std::error_code ec;
    if (!fs::exists(modDir, ec)) return ModType::Unknown;

    if (fs::exists(modDir / "red4ext" / "plugins", ec))                 return ModType::Red4ext;
    if (fs::exists(modDir / "bin" / "x64" / "plugins" / "cyber_engine_tweaks" / "mods", ec))
                                                                        return ModType::Cet;
    if (fs::exists(modDir / "r6" / "scripts", ec))                      return ModType::RedScript;
    if (fs::exists(modDir / "r6" / "tweaks", ec))                       return ModType::TweakXL;
    if (fs::exists(modDir / "archive" / "pc" / "mod", ec))              return ModType::Archive;
    return ModType::Unknown;
}

ModInventory EnumerateMO2(const fs::path& mo2Root) {
    ModInventory inv;
    inv.manager = ModManager::MO2;

    if (!fs::exists(mo2Root / "ModOrganizer.ini")) {
        inv.managerDetails = "ModOrganizer.ini missing";
        return inv;
    }

    std::string profile = ResolveMO2Profile(mo2Root);
    if (profile.empty()) {
        inv.managerDetails = "selected_profile unreadable";
        return inv;
    }

    fs::path modlistPath = mo2Root / "profiles" / profile / "modlist.txt";
    std::ifstream f(modlistPath, std::ios::binary);
    if (!f) {
        inv.managerDetails = "profile '" + profile + "' modlist.txt unreadable";
        return inv;
    }

    inv.managerDetails = "profile '" + profile + "'";

    std::string line;
    while (std::getline(f, line)) {
        std::string t = Trim(line);
        if (t.empty()) continue;
        char prefix = t[0];
        if (prefix == '#') continue;
        if (prefix != '+' && prefix != '-') continue;

        std::string name = Trim(std::string_view(t).substr(1));
        if (name.empty()) continue;
        if (name.size() >= 10 &&
            name.compare(name.size() - 10, 10, "_separator") == 0) continue;

        InstalledMod m;
        m.name    = name;
        m.enabled = (prefix == '+');
        fs::path modDir = mo2Root / "mods" / name;
        m.type    = ClassifyMO2Mod(modDir);

        m.version = ReadIniKey(modDir / "meta.ini", "General", "version");

        if (m.type == ModType::Red4ext) {
            std::string rel = FirstFileRelative(
                modDir, modDir / "red4ext" / "plugins", ".dll");
            if (!rel.empty()) m.subpath = std::move(rel);
        } else if (m.type == ModType::Asi) {
            std::string rel = FirstFileRelative(
                modDir, modDir / "bin" / "x64" / "plugins", ".asi");
            if (!rel.empty()) m.subpath = std::move(rel);
        } else if (m.type == ModType::Archive) {
            CollectArchivesInto(m.archives, modDir / "archive" / "pc" / "mod");
        }

        inv.mods.push_back(std::move(m));
    }

    ClampToCap(inv);
    return inv;
}

ModInventory EnumerateVortex(const fs::path& manifestPath) {
    ModInventory inv;
    inv.manager = ModManager::Vortex;

    try {
        simdjson::ondemand::parser parser;
        auto json = simdjson::padded_string::load(manifestPath.string());
        simdjson::ondemand::document doc = parser.iterate(json);

        std::unordered_map<std::string, std::string> firstPath;

        auto filesArr = doc["files"].get_array();
        for (auto entry : filesArr) {
            std::string_view srcSv;
            if (entry["source"].get_string().get(srcSv) != simdjson::SUCCESS) continue;
            std::string src(srcSv);
            if (firstPath.find(src) == firstPath.end()) {
                std::string_view relSv;
                if (entry["relPath"].get_string().get(relSv) == simdjson::SUCCESS) {
                    firstPath.emplace(std::move(src), std::string(relSv));
                } else {
                    firstPath.emplace(std::move(src), std::string());
                }
            }
        }

        inv.mods.reserve(firstPath.size());
        for (auto& kv : firstPath) {
            InstalledMod m;
            m.name    = kv.first;
            m.subpath = std::move(kv.second);
            m.enabled = true;
            m.type    = ModType::Unknown;
            inv.mods.push_back(std::move(m));
        }
        std::sort(inv.mods.begin(), inv.mods.end(),
            [](const InstalledMod& a, const InstalledMod& b){ return a.name < b.name; });

        inv.managerDetails = std::to_string(inv.mods.size()) + " mods deployed";
        ClampToCap(inv);
    } catch (const simdjson::simdjson_error& e) {
        inv.managerDetails = std::string("manifest unreadable: ") + e.what();
    } catch (...) {
        inv.managerDetails = "manifest unreadable: unknown error";
    }
    return inv;
}

void PushDirChildren(std::vector<InstalledMod>& out,
                     const fs::path& parent,
                     ModType type,
                     const std::string& subpathPrefix) {
    std::error_code ec;
    if (!fs::exists(parent, ec) || !fs::is_directory(parent, ec)) return;
    for (const auto& e : fs::directory_iterator(parent, ec)) {
        if (!e.is_directory(ec)) continue;
        InstalledMod m;
        m.name    = e.path().filename().string();
        m.type    = type;
        m.enabled = true;
        m.subpath = subpathPrefix + m.name;
        out.push_back(std::move(m));
    }
}

void PushFiles(std::vector<InstalledMod>& out,
               const fs::path& parent,
               std::string_view ext,
               ModType type,
               const std::string& subpathPrefix) {
    std::error_code ec;
    if (!fs::exists(parent, ec) || !fs::is_directory(parent, ec)) return;
    for (const auto& e : fs::directory_iterator(parent, ec)) {
        if (!e.is_regular_file(ec)) continue;
        auto p = e.path();
        if (!ExtEquals(p, ext)) continue;

        InstalledMod m;
        m.name    = p.stem().string();
        m.type    = type;
        m.enabled = true;
        m.subpath = subpathPrefix + p.filename().string();
        if (type == ModType::Archive) m.archives.push_back(p.filename().string());
        out.push_back(std::move(m));
    }
}

void PushRedsOrTweakDir(std::vector<InstalledMod>& out,
                        const fs::path& parent,
                        std::string_view ext,
                        ModType type,
                        const std::string& subpathPrefix) {
    std::error_code ec;
    if (!fs::exists(parent, ec) || !fs::is_directory(parent, ec)) return;
    for (const auto& e : fs::directory_iterator(parent, ec)) {
        if (e.is_directory(ec)) {
            InstalledMod m;
            m.name    = e.path().filename().string();
            m.type    = type;
            m.enabled = true;
            m.subpath = subpathPrefix + m.name;
            out.push_back(std::move(m));
        } else if (e.is_regular_file(ec)) {
            auto p = e.path();
            if (!ExtEquals(p, ext)) continue;
            InstalledMod m;
            m.name    = p.stem().string();
            m.type    = type;
            m.enabled = true;
            m.subpath = subpathPrefix + p.filename().string();
            out.push_back(std::move(m));
        }
    }
}

ModInventory EnumerateNative(const fs::path& gameRoot) {
    ModInventory inv;
    inv.manager = ModManager::None;
    inv.managerDetails = "native install";

    PushDirChildren(inv.mods,
                    gameRoot / "red4ext" / "plugins",
                    ModType::Red4ext,
                    "red4ext/plugins/");

    PushDirChildren(inv.mods,
                    gameRoot / "bin" / "x64" / "plugins" / "cyber_engine_tweaks" / "mods",
                    ModType::Cet,
                    "bin/x64/plugins/cyber_engine_tweaks/mods/");

    PushRedsOrTweakDir(inv.mods,
                       gameRoot / "r6" / "scripts",
                       ".reds",
                       ModType::RedScript,
                       "r6/scripts/");

    PushRedsOrTweakDir(inv.mods,
                       gameRoot / "r6" / "tweaks",
                       ".yaml",
                       ModType::TweakXL,
                       "r6/tweaks/");

    PushFiles(inv.mods,
              gameRoot / "archive" / "pc" / "mod",
              ".archive",
              ModType::Archive,
              "archive/pc/mod/");

    PushFiles(inv.mods,
              gameRoot / "bin" / "x64" / "plugins",
              ".asi",
              ModType::Asi,
              "bin/x64/plugins/");

    std::stable_sort(inv.mods.begin(), inv.mods.end(),
        [](const InstalledMod& a, const InstalledMod& b) {
            if (a.type != b.type) return (uint8_t)a.type < (uint8_t)b.type;
            return a.name < b.name;
        });

    ClampToCap(inv);
    return inv;
}

fs::path MO2RootFromUsvfsPath(const wchar_t* usvfsPath) {
    fs::path dll(usvfsPath);
    fs::path candidate = dll.has_parent_path() ? dll.parent_path() : dll;
    std::error_code ec;
    for (int i = 0; i < 3; ++i) {
        if (fs::exists(candidate / "ModOrganizer.ini", ec)) return candidate;
        if (!candidate.has_parent_path()) break;
        fs::path parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return {};
}

std::string NormRelKey(const fs::path& rel) {
    std::string s = rel.generic_string();
    if (s.size() > 5 &&
        (s[0] == 'R' || s[0] == 'r') && (s[1] == 'o' || s[1] == 'O') &&
        (s[2] == 'o' || s[2] == 'O') && (s[3] == 't' || s[3] == 'T') && s[4] == '/') {
        s = s.substr(5);
    }
    for (auto& c : s) { if (c == '/') c = '\\'; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); }
    return s;
}

void CollectTree(std::unordered_set<std::string>& out, const fs::path& base, size_t cap) {
    std::error_code ec;
    if (!fs::exists(base, ec) || !fs::is_directory(base, ec)) return;
    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         it != end && out.size() < cap; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        fs::path rel = it->path().lexically_relative(base);
        if (rel.empty()) continue;
        out.insert(NormRelKey(rel));
    }
}

fs::path ResolveMO2Dir(const fs::path& mo2Root, const fs::path& baseDir,
                       std::string_view key, const wchar_t* defSub) {
    std::string v = ReadIniKey(mo2Root / "ModOrganizer.ini", "Settings", key);
    if (v.empty()) return baseDir / defSub;
    const std::string token = "%BASE_DIR%";
    size_t pos = v.find(token);
    if (pos != std::string::npos) v.replace(pos, token.size(), baseDir.string());
    return fs::path(v);
}

}

std::filesystem::path TestMO2RootFromUsvfsPath(const wchar_t* usvfsPath) {
    return MO2RootFromUsvfsPath(usvfsPath);
}

ModInventory EnumerateInstalled(const fs::path& gameRoot,
                                const std::optional<fs::path>& mo2RootOverride) {
    if (mo2RootOverride.has_value()) {
        return EnumerateMO2(*mo2RootOverride);
    }
    HMODULE usvfs = ::GetModuleHandleW(L"usvfs_x64.dll");
    if (usvfs) {
        wchar_t buf[MAX_PATH] = {};
        DWORD n = ::GetModuleFileNameW(usvfs, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            fs::path root = MO2RootFromUsvfsPath(buf);
            if (!root.empty()) {
                return EnumerateMO2(root);
            }
            ModInventory inv;
            inv.manager = ModManager::MO2;
            inv.managerDetails = "usvfs loaded but MO2 root not found";
            return inv;
        }
        ModInventory inv;
        inv.manager = ModManager::MO2;
        inv.managerDetails = "usvfs loaded but path unresolvable";
        return inv;
    }

    fs::path vortexManifest = gameRoot / "__vortex_deployment.json";
    std::error_code ec;
    if (fs::exists(vortexManifest, ec)) {
        return EnumerateVortex(vortexManifest);
    }

    return EnumerateNative(gameRoot);
}

std::optional<std::unordered_set<std::string>> CollectMO2OverlayRelpaths(
    const std::optional<fs::path>& mo2RootOverride) {
    fs::path mo2Root;
    if (mo2RootOverride.has_value()) {
        mo2Root = *mo2RootOverride;
    } else {
        HMODULE usvfs = ::GetModuleHandleW(L"usvfs_x64.dll");
        if (!usvfs) return std::nullopt;
        wchar_t buf[MAX_PATH] = {};
        DWORD n = ::GetModuleFileNameW(usvfs, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return std::nullopt;
        mo2Root = MO2RootFromUsvfsPath(buf);
        if (mo2Root.empty()) return std::nullopt;
    }

    std::error_code ec;
    if (!fs::exists(mo2Root / "ModOrganizer.ini", ec)) return std::nullopt;

    std::string profile = ResolveMO2Profile(mo2Root);
    if (profile.empty()) return std::nullopt;

    std::ifstream f(mo2Root / "profiles" / profile / "modlist.txt", std::ios::binary);
    if (!f) return std::nullopt;

    std::string baseStr = ReadIniKey(mo2Root / "ModOrganizer.ini", "Settings", "base_directory");
    fs::path baseDir = baseStr.empty() ? mo2Root : fs::path(baseStr);
    fs::path modsDir = ResolveMO2Dir(mo2Root, baseDir, "mod_directory", L"mods");
    fs::path overwriteDir = ResolveMO2Dir(mo2Root, baseDir, "overwrite_directory", L"overwrite");

    std::unordered_set<std::string> set;
    const size_t kOverlayCap = 400000;

    std::string line;
    while (std::getline(f, line) && set.size() < kOverlayCap) {
        std::string t = Trim(line);
        if (t.empty() || t[0] != '+') continue;
        std::string name = Trim(std::string_view(t).substr(1));
        if (name.empty()) continue;
        if (name.size() >= 10 && name.compare(name.size() - 10, 10, "_separator") == 0) continue;
        CollectTree(set, modsDir / name, kOverlayCap);
    }
    CollectTree(set, overwriteDir, kOverlayCap);

    return set;
}

}

namespace redscope::report {
namespace {

const char* ManagerLabel(snap::ModManager m) {
    switch (m) {
        case snap::ModManager::None:   return "None";
        case snap::ModManager::MO2:    return "MO2";
        case snap::ModManager::Vortex: return "Vortex";
    }
    return "?";
}

}

void EmitInstalledMods(PreallocatedBuffer& out) {
    out.Append("--- Installed mods (summary; full per-mod list in .json sidecar) ---------------\n");
    const Snapshot* s = redscope::snap::Current();
    if (!s) {
        out.Append("(no inventory available)\n\n");
        return;
    }
    const auto& inv = s->inventory;

    out.Appendf("Manager: %s", ManagerLabel(inv.manager));
    if (!inv.managerDetails.empty()) {
        out.Appendf(" (%.*s)", (int)inv.managerDetails.size(), inv.managerDetails.data());
    }
    out.Append("\n");

    if (inv.mods.empty()) {
        out.Append("(no inventory available)\n\n");
        return;
    }

    uint32_t enabled = 0;
    for (const auto& m : inv.mods) {
        if (m.enabled) ++enabled;
    }
    const uint32_t total = (uint32_t)inv.mods.size();

    out.Appendf("%u installed, %u enabled%s\n", total, enabled,
                inv.truncated > 0 ? "  (capped at kMaxInstalledMods)" : "");
    out.Append("Loaded DLLs are under 'Loaded modules'; full per-mod list (name / version / enabled) is in the .json sidecar.\n\n");
}

}
