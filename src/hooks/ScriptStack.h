#pragma once
#include <cstddef>
#include <cstdint>

namespace redscope::scriptstack {

struct Frame {
    const char* fullName      = nullptr;
    const char* thisClassName = nullptr;
    uint64_t    funcHash      = 0;
    uint64_t    classHash     = 0;
    int64_t     enteredNs     = 0;
};

constexpr size_t kMaxDepth = 128;

struct ThreadStack {
    Frame    frames[kMaxDepth] = {};
    uint32_t depth             = 0;
    uint32_t overflow          = 0;
    uint32_t maxDepthSeen      = 0;
};

void Push(const Frame& f) noexcept;

void Pop() noexcept;

const ThreadStack& Current() noexcept;

ThreadStack& CurrentMutable() noexcept;

void ResetForTest() noexcept;

constexpr size_t kMaxHeartbeatThreads = 16;

struct HeartbeatSlot {
    uint32_t    threadId           = 0;
    const char* lastFullName       = nullptr;
    const char* lastThisClassName  = nullptr;
    uint64_t    lastFuncHash       = 0;
    uint64_t    lastClassHash      = 0;
    int64_t     lastEnteredNs      = 0;
};

void GetHeartbeatSlots(const HeartbeatSlot** outSlots, size_t* outCount) noexcept;

void GetHeartbeatSlotsMutable(HeartbeatSlot** outSlots, size_t* outCount) noexcept;

void TestWriteHeartbeat(size_t slotIndex, uint32_t threadId,
                        const char* name, int64_t enteredNs) noexcept;

void ResetHeartbeatsForTest() noexcept;

}
