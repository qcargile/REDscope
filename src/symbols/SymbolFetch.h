#pragma once
#include <filesystem>

namespace redscope::symfetch {

void StartAutoDownloadAsync(const std::filesystem::path& symbolsDir);
void RunDownloadBlocking(const std::filesystem::path& symbolsDir);

}
