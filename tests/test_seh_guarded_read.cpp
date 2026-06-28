#include <gtest/gtest.h>
#include "../src/util/SehGuardedRead.h"
#include "../src/util/PreallocatedBuffer.h"
#include <string>

TEST(SehSafeRead, FailsOnNullPointer) {
    int v = 0;
    EXPECT_FALSE(redscope::SehSafeRead(&v, nullptr, sizeof(int)));
}

TEST(SehSafeRead, SucceedsOnStackVar) {
    int src = 0xDEAD;
    int dst = 0;
    EXPECT_TRUE(redscope::SehSafeReadValue(dst, &src));
    EXPECT_EQ(dst, 0xDEAD);
}

TEST(PreallocatedBuffer, AppendWithinCapacity) {
    redscope::PreallocatedBuffer b;
    b.Reserve(64);
    EXPECT_TRUE(b.Append("hello"));
    EXPECT_TRUE(b.Appendf(" %d", 42));
    EXPECT_EQ(std::string(b.Data(), b.Size()), "hello 42");
    EXPECT_FALSE(b.Overflowed());
}

TEST(PreallocatedBuffer, OverflowSticky) {
    redscope::PreallocatedBuffer b;
    b.Reserve(8);
    EXPECT_TRUE(b.Append("12345"));
    EXPECT_FALSE(b.Append("6789ABCDEF"));
    EXPECT_TRUE(b.Overflowed());
    EXPECT_FALSE(b.Append("more"));
}
