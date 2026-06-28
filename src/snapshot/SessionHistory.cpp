#include "SessionHistory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace redscope::snap {

namespace {

constexpr const char* kMagic = "REDSCOPE_SESSION_PREVIOUS v1";

void SortByName(std::vector<ModSnapshot>& v) {
    std::sort(v.begin(), v.end(),
              [](const ModSnapshot& a, const ModSnapshot& b) {
                  return a.name < b.name;
              });
}

void StripCR(std::string& s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

}

PriorSession ReadSessionPrevious(const std::filesystem::path& path) {
    PriorSession out;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return out;
    out.present = true;

    std::ifstream in(path);
    if (!in) return out;

    std::string line;
    if (!std::getline(in, line)) return out;
    StripCR(line);
    if (line != kMagic) return out;

    while (std::getline(in, line)) {
        StripCR(line);
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        auto eq = line.find('=');
        if (line.rfind("timestampUnix=", 0) == 0 && eq != std::string::npos) {
            out.timestampUnix = std::strtoll(line.c_str() + eq + 1, nullptr, 10);
            continue;
        }
        auto bar = line.find('|');
        if (bar == std::string::npos) continue;
        ModSnapshot m;
        m.name    = line.substr(0, bar);
        m.version = line.substr(bar + 1);
        if (m.name.empty()) continue;
        out.mods.push_back(std::move(m));
    }
    SortByName(out.mods);
    out.parsedOk = true;
    return out;
}

bool WriteSessionPrevious(const std::filesystem::path& path,
                          int64_t timestampUnix,
                          const std::vector<ModSnapshot>& current) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    std::vector<ModSnapshot> sorted = current;
    SortByName(sorted);

    out << kMagic << '\n'
        << "timestampUnix=" << timestampUnix << '\n';
    for (const auto& m : sorted) {
        if (m.name.empty()) continue;
        out << m.name << '|' << m.version << '\n';
    }
    return out.good();
}

ModDiff ComputeModDiff(const std::vector<ModSnapshot>& prior,
                       const std::vector<ModSnapshot>& current) {
    ModDiff diff;
    diff.priorModCount = (uint32_t)prior.size();

    std::vector<ModSnapshot> a = prior;
    std::vector<ModSnapshot> b = current;
    SortByName(a);
    SortByName(b);

    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].name.empty()) { ++i; continue; }
        if (b[j].name.empty()) { ++j; continue; }
        if (a[i].name < b[j].name) {
            diff.removed.push_back(a[i++]);
        } else if (a[i].name > b[j].name) {
            diff.added.push_back(b[j++]);
        } else {
            if (a[i].version != b[j].version) {
                diff.updated.emplace_back(a[i], b[j]);
            }
            ++i; ++j;
        }
    }
    while (i < a.size()) {
        if (!a[i].name.empty()) diff.removed.push_back(a[i]);
        ++i;
    }
    while (j < b.size()) {
        if (!b[j].name.empty()) diff.added.push_back(b[j]);
        ++j;
    }
    return diff;
}

}
