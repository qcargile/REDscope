#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>

namespace redscope::logs {

std::vector<std::string> Tail(const std::filesystem::path& path, size_t maxLines);

std::vector<std::string> WindowAround(const std::filesystem::path& path,
                                      std::chrono::system_clock::time_point crashTime,
                                      int windowSec,
                                      size_t maxLines);

std::filesystem::path FindNewestMatching(const std::filesystem::path& dir,
                                         const std::string& glob);

}
