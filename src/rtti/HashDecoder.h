#pragma once
#include <cstddef>
#include <cstdint>

namespace redscope::rtti {

struct DecodedHash {
    char text[96] = {};
    bool decoded  = false;
};

using CNameGetFn = const char* (*)(uint64_t hash) noexcept;

using TweakDBIDLookupTypeFn = const char* (*)(uint64_t tdbId) noexcept;

struct HashResolvers {
    CNameGetFn             cnameGet     = nullptr;
    TweakDBIDLookupTypeFn  tdbIdLookup  = nullptr;
};

void                 SetHashResolvers(const HashResolvers& r) noexcept;
const HashResolvers& GetHashResolvers() noexcept;

DecodedHash TryDecodeCName(uint64_t hash) noexcept;

DecodedHash TryDecodeTweakDBID(uint64_t tdbId) noexcept;

void InstallSdkHashResolvers() noexcept;

}
