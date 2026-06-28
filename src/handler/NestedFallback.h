#pragma once
#include <windows.h>
#include <chrono>
#include <filesystem>

namespace redscope::handler {

bool RunWithNestedFallback(EXCEPTION_POINTERS* outerEp,
                           DWORD threadId,
                           const std::filesystem::path& crashesDir,
                           std::chrono::system_clock::time_point crashTime,
                           void (*body)(EXCEPTION_POINTERS*, DWORD, const std::filesystem::path&,
                                        std::chrono::system_clock::time_point));

}
