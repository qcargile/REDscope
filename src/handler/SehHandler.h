#pragma once
#include <filesystem>

namespace redscope::handler {

void Arm(const std::filesystem::path& crashesDir);
void Configure(bool respectDebugger, bool terminateAfterReport);
void Uninstall();

}
