#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace redscope::api {

std::string ProjectCrashSummary(const std::string& fullSidecarJson);

std::string BuildCrashArray(const std::vector<std::string>& objects);

std::string ReadAllCrashesJson(const std::filesystem::path& crashesDir, size_t cap);

}
