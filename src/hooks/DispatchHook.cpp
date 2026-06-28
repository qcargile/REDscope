#include "DispatchHook.h"
#include "ScriptStack.h"
#include "../Plugin.h"
#include "../Logger.h"
#include "../snapshot/RttiSnapshot.h"
#include "../util/Time.h"

#include <windows.h>
#include <atomic>
#include <cstdio>

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/CName.hpp>
#include <RED4ext/CNamePool.hpp>
#include <RED4ext/Detail/AddressHashes.hpp>
#include <RED4ext/Relocation.hpp>
#include <RED4ext/Scripting/Functions.hpp>
#include <RED4ext/Scripting/Stack.hpp>

namespace redscope::hooks::dispatch {

namespace {

using ExecuteScriptedFn_t = bool (*)(RED4ext::CBaseFunction*, RED4ext::CStack*, void*);

std::atomic<bool> g_installedScripted{false};
std::atomic<bool> g_scriptedRetryLogged{false};

void* g_scriptedTarget = nullptr;

ExecuteScriptedFn_t Original_ExecuteScripted = nullptr;

bool Detour_ExecuteScripted(RED4ext::CBaseFunction* fn, RED4ext::CStack* stack, void* extra) {
    scriptstack::Frame frame;
    frame.enteredNs = NowNs();
    if (fn) {
        frame.funcHash = fn->fullName.hash;
    }
    __try {
        if (stack) {
            RED4ext::IScriptable* ctx = stack->context18;
            if (!ctx) ctx = stack->context20;
            if (ctx) {
                uint64_t nativeType = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(ctx) + 0x30);
                if (nativeType) {
                    frame.classHash = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(nativeType) + 0x18);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    scriptstack::Push(frame);

    bool result = Original_ExecuteScripted(fn, stack, extra);

    scriptstack::Pop();
    return result;
}

}

void Install() {
    if (g_installedScripted.load(std::memory_order_acquire)) {
        return;
    }
    if (!redscope::snap::IsRttiReady()) return;

    if (!Sdk() || !PluginHandle() || !Sdk()->hooking) return;

    static RED4ext::UniversalRelocFunc<ExecuteScriptedFn_t> executeScriptedReloc(
        RED4ext::Detail::AddressHashes::CBaseFunction_ExecuteScripted);

    ExecuteScriptedFn_t scriptedAddr = executeScriptedReloc;

    if (!g_installedScripted.load(std::memory_order_acquire) && scriptedAddr) {
        void* target = reinterpret_cast<void*>(scriptedAddr);
        if (Sdk()->hooking->Attach(PluginHandle(), target,
                                    reinterpret_cast<void*>(&Detour_ExecuteScripted),
                                    reinterpret_cast<void**>(&Original_ExecuteScripted))) {
            g_scriptedTarget = target;
            g_installedScripted.store(true, std::memory_order_release);
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "DispatchHook: ExecuteScripted attached at 0x%p", target);
            log::Info(msg);
        } else if (!g_scriptedRetryLogged.exchange(true)) {
            log::Warn("DispatchHook: ExecuteScripted Attach failed");
        }
    }
}

void Uninstall() {
    if (g_installedScripted.exchange(false)) {
        if (Sdk() && PluginHandle() && Sdk()->hooking && g_scriptedTarget) {
            Sdk()->hooking->Detach(PluginHandle(), g_scriptedTarget);
        }
        g_scriptedTarget = nullptr;
    }
    log::Info("DispatchHook: detached");
}

void ResolveStackNames() noexcept {
    scriptstack::ThreadStack& s = scriptstack::CurrentMutable();
    uint32_t depth = s.depth;
    for (uint32_t i = 0; i < depth && i < scriptstack::kMaxDepth; ++i) {
        scriptstack::Frame& fr = s.frames[i];
        __try {
            if (fr.funcHash) {
                fr.fullName = RED4ext::CNamePool::Get(RED4ext::CName(fr.funcHash));
            }
            if (fr.classHash) {
                const char* nm = RED4ext::CNamePool::Get(RED4ext::CName(fr.classHash));
                if (nm && nm[0] != '\0') fr.thisClassName = nm;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

void ResolveHeartbeatNames() noexcept {
    scriptstack::HeartbeatSlot* slots = nullptr;
    size_t count = 0;
    scriptstack::GetHeartbeatSlotsMutable(&slots, &count);
    if (!slots) return;
    for (size_t i = 0; i < count; ++i) {
        if (slots[i].threadId == 0) continue;
        __try {
            if (slots[i].lastFuncHash) {
                slots[i].lastFullName = RED4ext::CNamePool::Get(RED4ext::CName(slots[i].lastFuncHash));
            }
            if (slots[i].lastClassHash) {
                const char* nm = RED4ext::CNamePool::Get(RED4ext::CName(slots[i].lastClassHash));
                if (nm && nm[0] != '\0') slots[i].lastThisClassName = nm;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

}
