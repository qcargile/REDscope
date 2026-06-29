#include "RttiSnapshot.h"
#include "../Logger.h"

#include <atomic>

#ifdef REDSCOPE_USE_RTTI
#include <RED4ext/RTTISystem.hpp>
#endif

namespace redscope::snap {
namespace {

std::atomic<bool> g_rttiReady{false};

}

bool IsRttiReady() noexcept {
    return g_rttiReady.load(std::memory_order_acquire);
}

#ifdef REDSCOPE_USE_RTTI

namespace {
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

#else

void RegisterRttiReadyCallback() {}

#endif

}
