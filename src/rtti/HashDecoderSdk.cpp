#include "HashDecoder.h"

#include <RED4ext/CName.hpp>
#include <RED4ext/CNamePool.hpp>
#include <RED4ext/NativeTypes.hpp>
#include <RED4ext/RTTITypes.hpp>
#include <RED4ext/Scripting/IScriptable.hpp>
#include <RED4ext/TweakDB.hpp>

namespace redscope::rtti {

namespace {

const char* SdkCNameGet(uint64_t hash) noexcept {
    return RED4ext::CNamePool::Get(RED4ext::CName(hash));
}

const char* SdkTweakDBIDLookupType(uint64_t id) noexcept {
    auto* tdb = RED4ext::TweakDB::Get();
    if (!tdb) return nullptr;

    RED4ext::Handle<RED4ext::IScriptable> record;
    if (!tdb->TryGetRecord(RED4ext::TweakDBID(id), record)) return nullptr;
    if (!record.instance) return nullptr;

    auto* type = record.instance->GetType();
    if (!type) return nullptr;

    return RED4ext::CNamePool::Get(type->GetName());
}

}

void InstallSdkHashResolvers() noexcept {
    HashResolvers r{};
    r.cnameGet    = &SdkCNameGet;
    r.tdbIdLookup = &SdkTweakDBIDLookupType;
    SetHashResolvers(r);
}

}
