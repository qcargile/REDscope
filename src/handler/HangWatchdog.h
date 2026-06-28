#pragma once

#include <cstdint>
#include <filesystem>

namespace redscope {
struct Snapshot;
}

namespace redscope::handler {

enum class HangVerdict : uint8_t {
    None     = 0,
    Detected = 1,
    Rearmed  = 2,
};

HangVerdict EvaluateHang(int64_t maxLastEnteredNs, int64_t nowNs,
                         int32_t engineStateRaw, int64_t thresholdNs,
                         bool alreadyReported) noexcept;

void CheckForHang(const redscope::Snapshot* snap,
                  const std::filesystem::path& crashesDir,
                  uint32_t thresholdSeconds,
                  bool enabled,
                  bool respectDebugger) noexcept;

}
