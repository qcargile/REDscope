#pragma once
#include <cstdint>
#include <filesystem>

namespace redscope::snap {

constexpr size_t kGpuHintPathCap = 260;
constexpr size_t kGpuHintCount   = 4;

struct GpuState {
    char hintLabels [kGpuHintCount][32]                  = {};
    char hintPaths  [kGpuHintCount][kGpuHintPathCap]     = {};
    bool hintExists [kGpuHintCount]                      = {};
};

void CaptureInto(GpuState& out, const std::filesystem::path& gameRoot) noexcept;

}
