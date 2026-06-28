#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace redscope::snap {

struct PluginInfo {
    std::string name;
    std::string version;
    std::string authors;
    std::string dllBasename;
};

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
    size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
    size_t operator()(const char* s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};

struct PluginMetadata {
    enum class Source : uint8_t {
        None        = 0,
        Red4extLog  = 1,
        NoLogFound  = 2,
        ParseError  = 3,
    };

    std::unordered_map<std::string, PluginInfo,
                       TransparentStringHash, std::equal_to<>> byDll;

    std::string cetVersion;
    std::string logTimestamp;
    uint32_t    failedLoadCount = 0;
    Source      source = Source::None;
};

PluginMetadata EnumeratePluginMetadata(const std::filesystem::path& gameRoot);

}
