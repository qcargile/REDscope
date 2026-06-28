#include "HashDecoder.h"
#include "../util/FixedStr.h"
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace redscope::rtti {

namespace {

HashResolvers g_resolvers = {};

constexpr uint64_t kCNameMinPlausibleHash = 0x0001'0000ull;

const char* SehCallCNameGet(CNameGetFn fn, uint64_t hash) noexcept {
    if (!fn) return nullptr;
    const char* result = nullptr;
    __try {
        result = fn(hash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = nullptr;
    }
    return result;
}

const char* SehCallTdbIdLookup(TweakDBIDLookupTypeFn fn, uint64_t id) noexcept {
    if (!fn) return nullptr;
    const char* result = nullptr;
    __try {
        result = fn(id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = nullptr;
    }
    return result;
}

}

void SetHashResolvers(const HashResolvers& r) noexcept {
    g_resolvers = r;
}

const HashResolvers& GetHashResolvers() noexcept {
    return g_resolvers;
}

DecodedHash TryDecodeCName(uint64_t hash) noexcept {
    DecodedHash out;
    if (hash < kCNameMinPlausibleHash) return out;
    if (!g_resolvers.cnameGet)         return out;

    const char* name = SehCallCNameGet(g_resolvers.cnameGet, hash);
    if (!name || name[0] == '\0')      return out;

    redscope::CopyFixed(out.text, sizeof(out.text), name);
    out.decoded = (out.text[0] != '\0');
    return out;
}

DecodedHash TryDecodeTweakDBID(uint64_t tdbId) noexcept {
    DecodedHash out;
    if (tdbId == 0) return out;

    const uint32_t hash   = static_cast<uint32_t>(tdbId & 0xFFFFFFFFull);
    const uint8_t  length = static_cast<uint8_t>((tdbId >> 32) & 0xFFull);

    if (hash == 0)   return out;
    if (length == 0) return out;

    if (!g_resolvers.tdbIdLookup) return out;

    const char* typeName = SehCallTdbIdLookup(g_resolvers.tdbIdLookup, tdbId);
    if (!typeName || typeName[0] == '\0') return out;

    std::snprintf(out.text, sizeof(out.text),
                  "TweakDBID(type=%s, len=%u, hash=0x%08X)",
                  typeName, (unsigned)length, (unsigned)hash);
    out.text[sizeof(out.text) - 1] = '\0';
    out.decoded = true;
    return out;
}

}
