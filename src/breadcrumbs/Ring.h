#pragma once
#include <atomic>
#include <array>
#include <type_traits>
#include <cstdint>

namespace redscope {

template <typename T, size_t Capacity>
class Ring {
    static_assert(Capacity > 0, "Capacity must be > 0");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    Ring() {
        for (auto& s : slots_) { s.seq.store(0, std::memory_order_relaxed); }
        head_.store(0, std::memory_order_relaxed);
    }

    void Push(const T& value) noexcept {
        uint64_t idx = head_.fetch_add(1, std::memory_order_release);
        auto& s = slots_[idx & (Capacity - 1)];
        uint64_t seq = s.seq.load(std::memory_order_relaxed);
        s.seq.store(seq + 1, std::memory_order_release);
        s.data = value;
        s.gen = idx;
        s.seq.store(seq + 2, std::memory_order_release);
    }

    template <typename Fn>
    void Snapshot(Fn&& fn) const noexcept {
        uint64_t head = head_.load(std::memory_order_acquire);
        uint64_t start = head > Capacity ? head - Capacity : 0;
        for (uint64_t i = start; i < head; ++i) {
            const auto& s = slots_[i & (Capacity - 1)];
            uint64_t s1 = s.seq.load(std::memory_order_acquire);
            if (s1 & 1) continue;
            T copy = s.data;
            uint64_t gen = s.gen;
            uint64_t s2 = s.seq.load(std::memory_order_acquire);
            if (s1 != s2 || gen != i) continue;
            fn(copy);
        }
    }

    uint64_t TotalPushes() const noexcept {
        return head_.load(std::memory_order_acquire);
    }

private:
    struct alignas(64) Slot {
        std::atomic<uint64_t> seq;
        uint64_t              gen = 0;
        T                     data{};
    };

    alignas(64) std::atomic<uint64_t> head_;
    alignas(64) std::array<Slot, Capacity> slots_;
};

}
