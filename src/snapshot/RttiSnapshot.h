#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace redscope {
class PreallocatedBuffer;
}

namespace redscope::snap {

struct ScriptedField {
    std::string name;
    std::string typeName;
    uint32_t    valueOffset  = 0;
    bool        isPersistent = false;
    bool        isSavable    = false;
};

struct ClassWithScriptedFields {
    std::string                name;
    std::string                parentName;
    uint32_t                   totalFieldCount      = 0;
    uint32_t                   scriptedFieldCount   = 0;
    std::vector<ScriptedField> fields;
};

struct RttiSnapshot {
    bool                                 ready                = false;
    uint32_t                             totalClassesWalked   = 0;
    uint32_t                             totalScriptedFields  = 0;
    std::vector<ClassWithScriptedFields> classes;
    uint32_t                             truncatedClasses     = 0;
    uint32_t                             truncatedFieldsPerClass = 0;
};

constexpr size_t kMaxClassesWithScriptedFields = 512;

constexpr size_t kMaxFieldsPerClass = 32;

RttiSnapshot CaptureRttiSnapshot();

bool IsRttiReady() noexcept;

void RegisterRttiReadyCallback();

RttiSnapshot BuildCappedSnapshotForTest(std::vector<ClassWithScriptedFields> classes,
                                        uint32_t totalClassesWalked);

const ClassWithScriptedFields* FindScriptedFieldClass(const RttiSnapshot& s,
                                                      const char* className) noexcept;

}

