#pragma once
#include <string_view>

namespace redscope::log {
    void Init(const wchar_t* logDir);
    void Shutdown();
    void Flush();
    void Info(std::string_view msg);
    void Warn(std::string_view msg);
    void Error(std::string_view msg);
}
