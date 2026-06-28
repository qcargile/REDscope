#include "Hooks.h"
#include "DispatchHook.h"
#include "../Logger.h"
#include <atomic>

namespace redscope::hooks {

namespace {
std::atomic<bool> g_completeLogged{false};
}

void InstallAll() {
    dispatch::Install();
    if (!g_completeLogged.exchange(true)) {
        log::Info("hooks::InstallAll first-pass complete.");
    }
}

void UninstallAll() {
    dispatch::Uninstall();
    log::Info("hooks::UninstallAll complete.");
}

}
