#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <filesystem>

namespace redscope::report {

class RsdbDict {
public:
    RsdbDict() = default;
    ~RsdbDict();
    RsdbDict(const RsdbDict&) = delete;
    RsdbDict& operator=(const RsdbDict&) = delete;

    bool Open(const std::filesystem::path& path) noexcept;
    bool IsOpen() const noexcept { return m_file != nullptr; }
    uint32_t Count() const noexcept { return m_count; }
    bool Lookup(uint64_t hash, std::string& out) const noexcept;

private:
    std::FILE* m_file = nullptr;
    uint32_t   m_count = 0;
    uint32_t   m_indexOffset = 0;
    uint32_t   m_pathsOffset = 0;
};

std::filesystem::path ResolveRsdbPath() noexcept;

}
