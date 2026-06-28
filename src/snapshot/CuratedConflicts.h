#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace redscope::snap {

struct ConflictModEntry {
    std::vector<std::string> archives;
    std::vector<std::string> tags;
};

struct ConflictDB {
    std::unordered_map<std::string, ConflictModEntry>         mods;
    std::unordered_map<std::string, std::vector<std::string>> dependencies;
    std::vector<std::vector<std::string>>                     conflicts;
    std::vector<std::vector<std::string>>                     exceptions;
    uint32_t filesMerged = 0;
    uint32_t parseErrors = 0;
};

struct ArchiveConflictGroup {
    std::vector<std::string> mods;
};

struct MissingDep {
    std::string              mod;
    std::vector<std::string> required;
};

struct ArchiveConflicts {
    std::vector<ArchiveConflictGroup> active;
    std::vector<MissingDep>           missingDeps;
    uint32_t                          dbModCount = 0;
    uint32_t                          truncated  = 0;
};

constexpr size_t kMaxConflictGroups = 64;
constexpr size_t kMaxMissingDeps    = 64;

bool MergeConflictJson(ConflictDB& db, const std::string& jsonText);

void FinalizeConflictDB(ConflictDB& db);

void LoadConflictDir(ConflictDB& db, const std::filesystem::path& dir);

ArchiveConflicts EvaluateConflicts(const ConflictDB& db,
                                   const std::vector<std::string>& installedArchives,
                                   bool isEP1,
                                   bool engineReadValid = true);

}
