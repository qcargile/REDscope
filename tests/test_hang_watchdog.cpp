#include <gtest/gtest.h>
#include "../src/handler/HangWatchdog.h"

using redscope::handler::EvaluateHang;
using redscope::handler::HangVerdict;

namespace {
constexpr int32_t kRunning = 3;
constexpr int64_t kThreshold = 45LL * 1'000'000'000LL;   // 45s in ns
constexpr int64_t kNow = 1'000'000'000'000LL;
}

TEST(HangWatchdog, DetectsWhenSilenceExceedsThresholdWhileRunning) {
    int64_t lastEntered = kNow - (60LL * 1'000'000'000LL);  // 60s ago
    EXPECT_EQ(EvaluateHang(lastEntered, kNow, kRunning, kThreshold, false),
              HangVerdict::Detected);
}

TEST(HangWatchdog, NoDetectionWhenWithinThreshold) {
    int64_t lastEntered = kNow - (10LL * 1'000'000'000LL);  // 10s ago
    EXPECT_EQ(EvaluateHang(lastEntered, kNow, kRunning, kThreshold, false),
              HangVerdict::None);
}

TEST(HangWatchdog, SuppressedWhenNotRunning) {
    int64_t lastEntered = kNow - (60LL * 1'000'000'000LL);  // would be a hang...
    EXPECT_EQ(EvaluateHang(lastEntered, kNow, 2 , kThreshold, false),
              HangVerdict::None);
    EXPECT_EQ(EvaluateHang(lastEntered, kNow, 0 , kThreshold, false),
              HangVerdict::None);
}

TEST(HangWatchdog, NoBaselineNoDetection) {
    EXPECT_EQ(EvaluateHang(0, kNow, kRunning, kThreshold, false), HangVerdict::None);
}

TEST(HangWatchdog, DoesNotReReportWhileStillHung) {
    int64_t lastEntered = kNow - (90LL * 1'000'000'000LL);  // still hung
    EXPECT_EQ(EvaluateHang(lastEntered, kNow, kRunning, kThreshold, true),
              HangVerdict::None);
}

TEST(HangWatchdog, RearmsWhenActivityResumes) {
    int64_t lastEntered = kNow - (2LL * 1'000'000'000LL);   // activity 2s ago
    EXPECT_EQ(EvaluateHang(lastEntered, kNow, kRunning, kThreshold, true),
              HangVerdict::Rearmed);
}

TEST(HangWatchdog, ExactThresholdIsNotYetAHang) {
    int64_t lastEntered = kNow - kThreshold;
    EXPECT_EQ(EvaluateHang(lastEntered, kNow, kRunning, kThreshold, false),
              HangVerdict::None);
}
