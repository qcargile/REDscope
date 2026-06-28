#pragma once
#include <cstdint>
#include <string_view>
#include <filesystem>

namespace redscope::symbols {

struct LookupResult {
    bool             found              = false;
    std::string_view symbol;
    uint32_t         offsetIntoFunction = 0;
};

void StartLoad(const std::filesystem::path& gameRoot);

bool Ready() noexcept;

LookupResult Lookup(uint32_t rvaInModule) noexcept;

size_t EntryCount() noexcept;

const char* Status() noexcept;

bool LookupByAddress(uintptr_t absoluteVa, char* nameOut, size_t nameCap,
                     uint32_t& offsetOut) noexcept;

bool ResolveCodeAddress(const char* moduleName, uintptr_t absoluteVa, uint32_t rva,
                        char* nameOut, size_t nameCap, uint32_t& offsetOut,
                        bool* approximate = nullptr) noexcept;

}
