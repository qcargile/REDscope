#include "RttiSnapshot.h"
#include "Snapshot.h"
#include "SnapshotWorker.h"
#include "../Logger.h"
#include "../report/Sections.h"
#include "../util/PreallocatedBuffer.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#ifdef REDSCOPE_USE_RTTI
#include <RED4ext/CName.hpp>
#include <RED4ext/Containers/DynArray.hpp>
#include <RED4ext/RTTISystem.hpp>
#include <RED4ext/RTTITypes.hpp>
#include <RED4ext/Scripting/CProperty.hpp>
#endif

namespace redscope::snap {
namespace {

std::atomic<bool> g_rttiReady{false};

void SortAndCap(RttiSnapshot& snap) {
    std::sort(snap.classes.begin(), snap.classes.end(),
        [](const ClassWithScriptedFields& a, const ClassWithScriptedFields& b) {
            if (a.scriptedFieldCount != b.scriptedFieldCount) {
                return a.scriptedFieldCount > b.scriptedFieldCount;
            }
            return a.name < b.name;
        });

    uint32_t truncatedFields = 0;
    for (auto& cls : snap.classes) {
        if (cls.fields.size() > kMaxFieldsPerClass) {
            truncatedFields += (uint32_t)(cls.fields.size() - kMaxFieldsPerClass);
            cls.fields.resize(kMaxFieldsPerClass);
        }
    }
    snap.truncatedFieldsPerClass = truncatedFields;

    if (snap.classes.size() > kMaxClassesWithScriptedFields) {
        snap.truncatedClasses =
            (uint32_t)(snap.classes.size() - kMaxClassesWithScriptedFields);
        snap.classes.resize(kMaxClassesWithScriptedFields);
    }

    snap.totalScriptedFields = 0;
    for (const auto& cls : snap.classes) {
        snap.totalScriptedFields += cls.scriptedFieldCount;
    }
}

}

const ClassWithScriptedFields* FindScriptedFieldClass(const RttiSnapshot& s,
                                                      const char* className) noexcept {
    if (!className || !className[0]) return nullptr;
    if (s.classes.empty())           return nullptr;
    for (const auto& c : s.classes) {
        if (c.scriptedFieldCount == 0)        break;
        if (c.name.size() == 0)               continue;
        if (std::strcmp(c.name.c_str(), className) == 0) return &c;
    }
    return nullptr;
}

RttiSnapshot BuildCappedSnapshotForTest(std::vector<ClassWithScriptedFields> classes,
                                        uint32_t totalClassesWalked) {
    RttiSnapshot snap;
    snap.ready = true;
    snap.totalClassesWalked = totalClassesWalked;
    snap.classes = std::move(classes);
    uint32_t total = 0;
    for (auto& c : snap.classes) {
        if (c.scriptedFieldCount == 0) c.scriptedFieldCount = (uint32_t)c.fields.size();
        total += c.scriptedFieldCount;
    }
    snap.totalScriptedFields = total;
    SortAndCap(snap);
    return snap;
}

bool IsRttiReady() noexcept {
    return g_rttiReady.load(std::memory_order_acquire);
}

#ifdef REDSCOPE_USE_RTTI

namespace {

std::string ResolveName(RED4ext::CName name) {
    const char* s = name.ToString();
    return s ? std::string(s) : std::string();
}

std::string ResolveTypeName(RED4ext::rtti::IType* type) {
    if (!type) return {};
    return ResolveName(type->GetName());
}

void PostRegisterCallback() {
    g_rttiReady.store(true, std::memory_order_release);
    redscope::log::Info("RTTI: PostRegisterTypes callback fired; readiness armed");
}

}

void RegisterRttiReadyCallback() {
    auto* rtti = RED4ext::CRTTISystem::Get();
    if (!rtti) return;
    rtti->AddPostRegisterCallback(&PostRegisterCallback);
}

RttiSnapshot CaptureRttiSnapshot() {
    RttiSnapshot snap;
    if (!IsRttiReady()) return snap;

    auto* rtti = RED4ext::CRTTISystem::Get();
    if (!rtti) return snap;

    RED4ext::DynArray<RED4ext::CClass*> classes;
    rtti->GetClasses(nullptr, classes, nullptr, true);

    snap.ready = true;
    snap.totalClassesWalked = classes.Size();

    for (uint32_t i = 0; i < classes.Size(); ++i) {
        RED4ext::CClass* cls = classes[i];
        if (!cls) continue;

        ClassWithScriptedFields entry;
        entry.totalFieldCount = cls->unk118.Size();

        for (uint32_t j = 0; j < cls->props.Size(); ++j) {
            RED4ext::CProperty* prop = cls->props[j];
            if (!prop) continue;
            if (!prop->flags.isScripted) continue;

            ScriptedField f;
            f.name         = ResolveName(prop->name);
            f.typeName     = ResolveTypeName(prop->type);
            f.valueOffset  = prop->valueOffset;
            f.isPersistent = prop->flags.isPersistent;
            f.isSavable    = prop->flags.isSavable;
            entry.fields.push_back(std::move(f));
        }

        if (entry.fields.empty()) continue;

        entry.name               = ResolveName(cls->name);
        entry.parentName         = cls->parent ? ResolveName(cls->parent->name) : std::string();
        entry.scriptedFieldCount = (uint32_t)entry.fields.size();
        snap.totalScriptedFields += entry.scriptedFieldCount;
        snap.classes.push_back(std::move(entry));
    }

    SortAndCap(snap);
    return snap;
}

#else

void RegisterRttiReadyCallback() {}

RttiSnapshot CaptureRttiSnapshot() {
    return RttiSnapshot{};
}

#endif

}

namespace redscope::report {


}
