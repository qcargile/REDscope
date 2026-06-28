#include "ScriptStack.h"
#include <windows.h>
#include <atomic>

namespace redscope::scriptstack {

namespace {
thread_local ThreadStack g_stack;

HeartbeatSlot  g_heartbeats[kMaxHeartbeatThreads] = {};
thread_local int t_slotIndex = -1;

int ClaimHeartbeatSlot(uint32_t tid) noexcept {
    for (size_t i = 0; i < kMaxHeartbeatThreads; ++i) {
        uint32_t expected = 0;
        auto* tidPtr = reinterpret_cast<std::atomic<uint32_t>*>(
            &g_heartbeats[i].threadId);
        if (tidPtr->compare_exchange_strong(expected, tid,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
            return (int)i;
        }
    }
    return -1;
}

void UpdateHeartbeat(const Frame& f) noexcept {
    if (t_slotIndex < 0) {
        t_slotIndex = ClaimHeartbeatSlot(::GetCurrentThreadId());
    }
    if (t_slotIndex < 0) return;
    g_heartbeats[t_slotIndex].lastFuncHash  = f.funcHash;
    g_heartbeats[t_slotIndex].lastClassHash = f.classHash;
    g_heartbeats[t_slotIndex].lastEnteredNs = f.enteredNs;
}
}

void Push(const Frame& f) noexcept {
    UpdateHeartbeat(f);
    if (g_stack.depth < kMaxDepth) {
        g_stack.frames[g_stack.depth++] = f;
        if (g_stack.depth > g_stack.maxDepthSeen) {
            g_stack.maxDepthSeen = g_stack.depth;
        }
    } else {
        ++g_stack.overflow;
    }
}

void Pop() noexcept {
    if (g_stack.overflow > 0) {
        --g_stack.overflow;
    } else if (g_stack.depth > 0) {
        --g_stack.depth;
    }
}

const ThreadStack& Current() noexcept {
    return g_stack;
}

ThreadStack& CurrentMutable() noexcept {
    return g_stack;
}

void ResetForTest() noexcept {
    g_stack.depth        = 0;
    g_stack.overflow     = 0;
    g_stack.maxDepthSeen = 0;
}

void GetHeartbeatSlots(const HeartbeatSlot** outSlots, size_t* outCount) noexcept {
    if (outSlots) *outSlots = g_heartbeats;
    if (outCount) *outCount = kMaxHeartbeatThreads;
}

void GetHeartbeatSlotsMutable(HeartbeatSlot** outSlots, size_t* outCount) noexcept {
    if (outSlots) *outSlots = g_heartbeats;
    if (outCount) *outCount = kMaxHeartbeatThreads;
}

void TestWriteHeartbeat(size_t slotIndex, uint32_t threadId,
                        const char* name, int64_t enteredNs) noexcept {
    if (slotIndex >= kMaxHeartbeatThreads) return;
    g_heartbeats[slotIndex].threadId          = threadId;
    g_heartbeats[slotIndex].lastFullName      = name;
    g_heartbeats[slotIndex].lastThisClassName = nullptr;
    g_heartbeats[slotIndex].lastEnteredNs     = enteredNs;
}

void ResetHeartbeatsForTest() noexcept {
    for (size_t i = 0; i < kMaxHeartbeatThreads; ++i) {
        g_heartbeats[i] = {};
    }
    t_slotIndex = -1;
}

}
