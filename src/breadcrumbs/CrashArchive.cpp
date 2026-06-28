#include "CrashArchive.h"

#include <simdjson.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

namespace redscope::api {

namespace {

void AppendEscaped(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

void AppendJsonStr(std::string& out, std::string_view s) {
    out += '"';
    AppendEscaped(out, s);
    out += '"';
}

}

std::string ProjectCrashSummary(const std::string& fullSidecarJson) {
    if (fullSidecarJson.empty()) return "";
    try {
        simdjson::dom::parser parser;
        simdjson::dom::element doc;
        if (parser.parse(simdjson::padded_string(fullSidecarJson)).get(doc) != simdjson::SUCCESS) {
            return "";
        }

        auto str = [&](const char* key) -> std::string {
            std::string_view sv;
            if (doc[key].get(sv) == simdjson::SUCCESS) return std::string(sv);
            return "";
        };

        std::string crashId = str("crashId");
        std::string looseId = str("looseId");
        std::string crashClass = str("crashClass");

        uint64_t crashTime = 0;
        if (doc["crashTimeUnix"].get(crashTime) != simdjson::SUCCESS) {
            int64_t signedTime = 0;
            if (doc["crashTimeUnix"].get(signedTime) == simdjson::SUCCESS && signedTime > 0) {
                crashTime = static_cast<uint64_t>(signedTime);
            }
        }

        std::string faultName;
        {
            simdjson::dom::element fm;
            if (doc["faultingModule"].get(fm) == simdjson::SUCCESS) {
                std::string_view sv;
                if (fm["name"].get(sv) == simdjson::SUCCESS) faultName = std::string(sv);
            }
        }

        bool faultSiteUnreliable = false;
        {
            bool b = false;
            if (doc["fingerprint"]["faultSiteUnreliable"].get(b) == simdjson::SUCCESS) {
                faultSiteUnreliable = b;
            }
        }

        std::string out;
        out.reserve(256);
        out += "{\"crashId\":";        AppendJsonStr(out, crashId);
        out += ",\"looseId\":";        AppendJsonStr(out, looseId);
        out += ",\"crashTimeUnix\":";  out += std::to_string(crashTime);
        out += ",\"crashClass\":";     AppendJsonStr(out, crashClass);
        out += ",\"faultingModule\":";
        if (faultName.empty()) out += "null"; else AppendJsonStr(out, faultName);
        out += ",\"faultSiteUnreliable\":"; out += (faultSiteUnreliable ? "true" : "false");
        out += ",\"stackModules\":[";

        simdjson::dom::array arr;
        if (doc["stackModules"].get(arr) == simdjson::SUCCESS) {
            bool first = true;
            for (simdjson::dom::element el : arr) {
                std::string_view name;
                if (el["name"].get(name) != simdjson::SUCCESS) continue;
                uint64_t hits = 0;
                el["hits"].get(hits);
                std::string_view kind;
                el["kind"].get(kind);
                if (!first) out += ",";
                first = false;
                out += "{\"name\":"; AppendJsonStr(out, name);
                out += ",\"hits\":";  out += std::to_string(hits);
                out += ",\"kind\":";  AppendJsonStr(out, kind);
                out += "}";
            }
        }

        out += "]}";
        return out;
    } catch (...) {
        return "";
    }
}

std::string BuildCrashArray(const std::vector<std::string>& objects) {
    std::string out = "[";
    bool first = true;
    for (const auto& o : objects) {
        if (o.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        if (!first) out += ",";
        first = false;
        out += o;
    }
    out += "]";
    return out;
}

std::string ReadAllCrashesJson(const std::filesystem::path& crashesDir, size_t cap) {
    std::vector<std::string> summaries;
    try {
        std::error_code ec;
        if (!std::filesystem::exists(crashesDir, ec)) return "[]";

        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
        for (const auto& entry : std::filesystem::directory_iterator(crashesDir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const auto& p = entry.path();
            if (p.extension() != ".json") continue;
            if (p.stem().extension() != ".crash") continue;
            auto t = std::filesystem::last_write_time(p, ec);
            if (ec) continue;
            files.emplace_back(t, p);
        }

        std::sort(files.begin(), files.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        if (cap > 0 && files.size() > cap) files.resize(cap);

        for (const auto& entry : files) {
            std::ifstream f(entry.second, std::ios::binary);
            if (!f) continue;
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string proj = ProjectCrashSummary(ss.str());
            if (!proj.empty()) summaries.push_back(std::move(proj));
        }
    } catch (...) {
    }
    return BuildCrashArray(summaries);
}

}
