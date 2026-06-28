#include "../src/breadcrumbs/BreadcrumbStore.h"
#include "../src/breadcrumbs/Breadcrumb.h"

#include <chrono>
#include <cstdio>

namespace {

template <typename F>
double MeasureNsPerOp(size_t iters, F&& fn) {
    using namespace std::chrono;
    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < iters; ++i) fn(i);
    auto end = high_resolution_clock::now();
    auto ns = duration_cast<nanoseconds>(end - start).count();
    return (double)ns / (double)iters;
}

int BenchCrumb() {
    redscope::InitBreadcrumbStore();

    constexpr size_t kWarmup = 100'000;
    for (size_t i = 0; i < kWarmup; ++i) {
        redscope::Crumb(redscope::BcUser, "warmup", "msg");
    }

    constexpr size_t kIters = 5'000'000;
    double ns = MeasureNsPerOp(kIters, [](size_t i) {
        char msg[16];
        std::snprintf(msg, sizeof(msg), "n=%zu", i & 0xFFFF);
        redscope::Crumb(redscope::BcUser, "bench", msg);
    });

    std::printf("Crumb()            : %.1f ns/op  (target < 100 ns)  %s\n",
                ns, ns < 100.0 ? "PASS" : "FAIL");
    return ns < 100.0 ? 0 : 1;
}

int BenchRingSnapshot() {
    redscope::InitBreadcrumbStore();
    auto& store = redscope::GetBreadcrumbStore();

    for (size_t i = 0; i < 300; ++i) {
        redscope::Crumb(redscope::BcUser, "fill", "dummy");
    }

    constexpr size_t kIters = 10'000;
    double ns = MeasureNsPerOp(kIters, [&](size_t) {
        size_t count = 0;
        store.ring.Snapshot([&count](const redscope::Breadcrumb&) { ++count; });
        if (count == 0xDEADBEEF) std::printf("!");
    });

    constexpr double kTargetUs = 1000.0;
    double us = ns / 1000.0;
    std::printf("Ring::Snapshot (256): %.2f us/op (target < %.0f us) %s\n",
                us, kTargetUs, us < kTargetUs ? "PASS" : "FAIL");
    return us < kTargetUs ? 0 : 1;
}

}

int main() {
    std::printf("REDscope perf bench\n");
    std::printf("-------------------\n");
    int rc = 0;
    rc |= BenchCrumb();
    rc |= BenchRingSnapshot();
    return rc;
}
