#include "CuratedConflicts.h"

#include <simdjson.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace redscope::snap {

namespace {

const char* const kPhantomLiberty = "Phantom Liberty";

std::string Lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return r;
}

bool EndsWithArchive(const std::string& s) {
    static const std::string ext = ".archive";
    if (s.size() < ext.size()) return false;
    return Lower(s.substr(s.size() - ext.size())) == ext;
}

void AddElement(ConflictModEntry& e, std::string_view raw) {
    std::string v(raw);
    if (EndsWithArchive(v)) {
        e.archives.push_back(Lower(v));
    } else {
        e.tags.push_back(std::move(v));
    }
}

std::string PairKey(const std::string& a, const std::string& b) {
    return a < b ? a + '\x01' + b : b + '\x01' + a;
}

}

bool MergeConflictJson(ConflictDB& db, const std::string& jsonText) {
    if (jsonText.empty()) return false;
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(simdjson::padded_string(jsonText)).get(doc) != simdjson::SUCCESS) {
        ++db.parseErrors;
        return false;
    }

    simdjson::dom::object modsObj;
    if (doc["mods"].get(modsObj) == simdjson::SUCCESS) {
        for (auto field : modsObj) {
            std::string name(field.key);
            if (db.mods.find(name) != db.mods.end()) continue;

            ConflictModEntry entry;
            std::string_view sv;
            simdjson::dom::array arr;
            if (field.value.get(sv) == simdjson::SUCCESS) {
                AddElement(entry, sv);
            } else if (field.value.get(arr) == simdjson::SUCCESS) {
                for (auto el : arr) {
                    std::string_view es;
                    if (el.get(es) == simdjson::SUCCESS) AddElement(entry, es);
                }
            } else {
                continue;
            }
            db.mods.emplace(std::move(name), std::move(entry));
        }
    }

    simdjson::dom::object depsObj;
    if (doc["dependencies"].get(depsObj) == simdjson::SUCCESS) {
        for (auto field : depsObj) {
            simdjson::dom::array arr;
            if (field.value.get(arr) != simdjson::SUCCESS) continue;
            std::vector<std::string> reqs;
            for (auto el : arr) {
                std::string_view es;
                if (el.get(es) == simdjson::SUCCESS) reqs.emplace_back(es);
            }
            db.dependencies[std::string(field.key)] = std::move(reqs);
        }
    }

    auto loadGroups = [&](const char* key, std::vector<std::vector<std::string>>& out) {
        simdjson::dom::array groups;
        if (doc[key].get(groups) != simdjson::SUCCESS) return;
        for (auto g : groups) {
            simdjson::dom::array grp;
            if (g.get(grp) != simdjson::SUCCESS) continue;
            std::vector<std::string> members;
            for (auto el : grp) {
                std::string_view es;
                if (el.get(es) == simdjson::SUCCESS) members.emplace_back(es);
            }
            if (members.size() >= 2) out.push_back(std::move(members));
        }
    };
    loadGroups("conflicts", db.conflicts);
    loadGroups("exceptions", db.exceptions);

    ++db.filesMerged;
    return true;
}

void FinalizeConflictDB(ConflictDB& db) {
    std::unordered_map<std::string, std::vector<std::string>> tagGroups;
    for (const auto& [name, entry] : db.mods) {
        for (const auto& tag : entry.tags) tagGroups[tag].push_back(name);
    }
    std::vector<std::string> tagNames;
    for (auto& [tag, members] : tagGroups) {
        if (members.size() >= 2) tagNames.push_back(tag);
    }
    std::sort(tagNames.begin(), tagNames.end());
    for (const auto& tag : tagNames) {
        auto& members = tagGroups[tag];
        std::sort(members.begin(), members.end());
        db.conflicts.push_back(members);
    }

    for (auto& grp : db.conflicts) {
        grp.erase(std::remove_if(grp.begin(), grp.end(),
            [&](const std::string& m) { return db.mods.find(m) == db.mods.end(); }),
            grp.end());
    }
    db.conflicts.erase(std::remove_if(db.conflicts.begin(), db.conflicts.end(),
        [](const std::vector<std::string>& g) { return g.size() < 2; }),
        db.conflicts.end());

    for (auto it = db.dependencies.begin(); it != db.dependencies.end();) {
        if (db.mods.find(it->first) == db.mods.end()) {
            it = db.dependencies.erase(it);
            continue;
        }
        auto& reqs = it->second;
        reqs.erase(std::remove_if(reqs.begin(), reqs.end(),
            [&](const std::string& r) {
                return r != kPhantomLiberty && db.mods.find(r) == db.mods.end();
            }), reqs.end());
        if (reqs.empty()) {
            it = db.dependencies.erase(it);
            continue;
        }
        ++it;
    }
}

void LoadConflictDir(ConflictDB& db, const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) return;

    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const auto& p = e.path();
        if (p.extension() != ".json") continue;
        std::string fn = p.filename().string();
        if (!fn.empty() && fn[0] == '_') continue;
        files.push_back(p);
    }
    std::sort(files.begin(), files.end());

    for (const auto& p : files) {
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        MergeConflictJson(db, ss.str());
    }
    FinalizeConflictDB(db);
}

ArchiveConflicts EvaluateConflicts(const ConflictDB& db,
                                   const std::vector<std::string>& installedArchives,
                                   bool isEP1,
                                   bool engineReadValid) {
    ArchiveConflicts result;
    result.dbModCount = (uint32_t)db.mods.size();

    std::set<std::string> archiveSet;
    for (const auto& a : installedArchives) archiveSet.insert(Lower(a));

    std::unordered_map<std::string, bool> installed;
    installed.reserve(db.mods.size());
    for (const auto& [name, entry] : db.mods) {
        bool present = false;
        for (const auto& a : entry.archives) {
            if (archiveSet.count(a)) { present = true; break; }
        }
        installed[name] = present;
    }

    std::set<std::string> exceptedPairs;
    for (const auto& grp : db.exceptions) {
        for (size_t i = 0; i < grp.size(); ++i) {
            for (size_t j = i + 1; j < grp.size(); ++j) {
                exceptedPairs.insert(PairKey(grp[i], grp[j]));
            }
        }
    }
    auto isExcepted = [&](const std::string& a, const std::string& b) {
        return exceptedPairs.count(PairKey(a, b)) > 0;
    };

    for (const auto& grp : db.conflicts) {
        std::vector<std::string> members;
        for (const auto& m : grp) {
            auto it = installed.find(m);
            if (it != installed.end() && it->second) members.push_back(m);
        }
        if (members.size() < 2) continue;

        std::vector<std::string> survivors;
        for (const auto& a : members) {
            bool stillConflicts = false;
            for (const auto& b : members) {
                if (a != b && !isExcepted(a, b)) { stillConflicts = true; break; }
            }
            if (stillConflicts) survivors.push_back(a);
        }
        if (survivors.size() >= 2) {
            if (result.active.size() >= kMaxConflictGroups) { ++result.truncated; continue; }
            std::sort(survivors.begin(), survivors.end());
            result.active.push_back({ std::move(survivors) });
        }
    }

    for (const auto& [mod, reqs] : db.dependencies) {
        auto it = installed.find(mod);
        if (it == installed.end() || !it->second) continue;
        std::vector<std::string> unmet;
        for (const auto& r : reqs) {
            bool met;
            if (r == kPhantomLiberty) {
                if (!engineReadValid) continue;
                met = isEP1;
            } else {
                auto ri = installed.find(r);
                met = (ri != installed.end() && ri->second);
            }
            if (!met) unmet.push_back(r);
        }
        if (!unmet.empty()) {
            if (result.missingDeps.size() >= kMaxMissingDeps) { ++result.truncated; continue; }
            std::sort(unmet.begin(), unmet.end());
            result.missingDeps.push_back({ mod, std::move(unmet) });
        }
    }

    std::sort(result.active.begin(), result.active.end(),
        [](const ArchiveConflictGroup& a, const ArchiveConflictGroup& b) {
            return a.mods < b.mods;
        });
    std::sort(result.missingDeps.begin(), result.missingDeps.end(),
        [](const MissingDep& a, const MissingDep& b) { return a.mod < b.mod; });

    return result;
}

}
