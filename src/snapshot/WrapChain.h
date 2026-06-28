#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "InstalledMods.h"

namespace redscope::snap {

constexpr size_t kWrapMethodKeyCap  = 80;
constexpr size_t kWrapModNameCap    = 32;
constexpr size_t kWrapRelFileCap    = 96;
constexpr size_t kMaxWrapLayers     = 6;
constexpr size_t kMaxWrapChains     = 512;

struct WrapLayer {
    char     modName[kWrapModNameCap] = {};
    char     relFile[kWrapRelFileCap] = {};
    uint32_t lineNumber               = 0;
};

struct WrapChainEntry {
    char      methodKey[kWrapMethodKeyCap] = {};
    uint32_t  layerCount                   = 0;
    uint32_t  extraLayers                  = 0;
    WrapLayer layers[kMaxWrapLayers]       = {};
};

struct WrapChainTable {
    std::vector<WrapChainEntry> chains;
    uint32_t                    chainsDropped = 0;
    uint32_t                    totalLayers   = 0;
    uint32_t                    filesScanned  = 0;
    uint32_t                    modsWithWraps = 0;
    bool                        scanPerformed = false;
};

WrapChainTable CaptureWrapChains(const std::filesystem::path& gameRoot,
                                 const ModInventory& inventory);

const WrapChainEntry* FindChain(const WrapChainTable& t,
                                const char* classDotMethod) noexcept;

bool ParseWrapAnnotation(const char* line, std::string& outClass);

bool ParseFuncDeclaration(const char* line, std::string& outMethod);

void BuildMethodKey(const char* cls, const char* meth,
                    char* out, size_t cap);

void NormalizeWrapMethodKey(const char* fullName, char* key, size_t cap);

}
