#include <gtest/gtest.h>
#include "../src/breadcrumbs/Ring.h"
#include <atomic>
#include <thread>
#include <vector>

struct Entry {
    int64_t ts;
    int     tag;
    int     value;
};

TEST(Ring, EmptyInitiallyAndSingleProducerSingleConsumer) {
    redscope::Ring<Entry, 8> r;
    std::vector<Entry> out;
    r.Snapshot([&](const Entry& e) { out.push_back(e); });
    EXPECT_TRUE(out.empty());

    r.Push({1, 100, 42});
    r.Push({2, 101, 43});
    out.clear();
    r.Snapshot([&](const Entry& e) { out.push_back(e); });
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].ts, 1);
    EXPECT_EQ(out[0].value, 42);
    EXPECT_EQ(out[1].ts, 2);
}

TEST(Ring, WrapsAroundKeepingNewest) {
    redscope::Ring<Entry, 4> r;
    for (int i = 1; i <= 10; ++i) r.Push({i, 0, i * 10});
    std::vector<Entry> out;
    r.Snapshot([&](const Entry& e) { out.push_back(e); });
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out.front().ts, 7);
    EXPECT_EQ(out.back().ts, 10);
}

TEST(Ring, ConcurrentProducerDoesNotCrashConsumer) {
    redscope::Ring<Entry, 256> r;
    std::atomic<bool> stop{false};
    std::thread prod([&]{
        int64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            r.Push({++i, 1, (int)i});
        }
    });
    for (int pass = 0; pass < 1000; ++pass) {
        std::vector<Entry> out;
        r.Snapshot([&](const Entry& e) { out.push_back(e); });
        EXPECT_LE(out.size(), 256u);
    }
    stop.store(true);
    prod.join();
}
