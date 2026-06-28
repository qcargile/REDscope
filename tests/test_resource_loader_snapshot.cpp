#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include <cstdint>
#include "snapshot/ResourceLoaderSnapshot.h"

using redscope::snap::ParseResourceLoader;
using redscope::snap::ResourceLoaderSnapshot;

namespace {

struct BufferReader : redscope::snap::ResourceLoaderMemReader {
    const uint8_t* base;
    size_t len;
    BufferReader(const uint8_t* b, size_t l) : base(b), len(l) {}
    bool InRange(const void* addr, size_t n) const noexcept {
        auto a = reinterpret_cast<const uint8_t*>(addr);
        if (!base || a < base) return false;
        if (a + n < a) return false;
        return a + n <= base + len;
    }
    bool Read(const void* addr, void* dst, size_t n) const noexcept override {
        if (!InRange(addr, n)) return false;
        std::memcpy(dst, addr, n);
        return true;
    }
    bool Readable(const void* addr, size_t n) const noexcept override {
        return InRange(addr, n);
    }
};

constexpr size_t kFailedArr = 0x100;
constexpr size_t kTokenA    = 0x140;
constexpr size_t kTokenB    = 0x1A0;
constexpr size_t kIdxTable  = 0x200;
constexpr size_t kNodes     = 0x220;
constexpr size_t kBufSize   = 0x300;

}

TEST(ResourceLoaderSnapshot, FailedAndInFlight) {
    std::vector<uint8_t> buf(kBufSize, 0);
    uint8_t* base = buf.data();
    auto putU32 = [&](size_t off, uint32_t v) { std::memcpy(base + off, &v, sizeof(v)); };
    auto putU64 = [&](size_t off, uint64_t v) { std::memcpy(base + off, &v, sizeof(v)); };
    auto putU8  = [&](size_t off, uint8_t v)  { base[off] = v; };
    auto putPtr = [&](size_t off, size_t targetOff) {
        uint64_t p = reinterpret_cast<uint64_t>(base + targetOff);
        std::memcpy(base + off, &p, sizeof(p));
    };

    putPtr(0x30, kFailedArr);  // failed.m_entries
    putU32(0x3C, 2);           // failed.m_size
    putPtr(0x00, kIdxTable);   // tokens.indexTable
    putU32(0x08, 2);           // tokens.size (map entries)
    putU32(0x0C, 4);           // tokens.capacity (bucket count)
    putPtr(0x10, kNodes);      // tokens.nodeList.nodes
    putU32(0x18, 4);           // tokens.nodeList.capacity

    putPtr(kFailedArr + 0x00, kTokenA);  // SharedPtr[0].instance
    putPtr(kFailedArr + 0x10, kTokenB);  // SharedPtr[1].instance
    putU64(kTokenA + 0x48, 0xAAAA1111AAAA1111ull);
    putU8 (kTokenA + 0x5C, 1);
    putU64(kTokenB + 0x48, 0xBBBB2222BBBB2222ull);
    putU8 (kTokenB + 0x5C, 3);

    putU32(kIdxTable + 0x00, 0);           // bucket 0 -> node 0
    putU32(kIdxTable + 0x04, 0xFFFFFFFF);
    putU32(kIdxTable + 0x08, 0xFFFFFFFF);
    putU32(kIdxTable + 0x0C, 0xFFFFFFFF);
    putU32(kNodes + 0x00, 1);                       // node 0 next = 1
    putU64(kNodes + 0x08, 0xCCCC3333CCCC3333ull);   // node 0 key
    putU32(kNodes + 0x20, 0xFFFFFFFF);              // node 1 next = invalid
    putU64(kNodes + 0x28, 0xDDDD4444DDDD4444ull);   // node 1 key

    BufferReader reader(base, kBufSize);
    ResourceLoaderSnapshot out;
    ParseResourceLoader(base, reader, out);

    EXPECT_TRUE(out.readOk);
    EXPECT_TRUE(out.hasData);
    EXPECT_EQ(out.failedTotal, 2u);
    EXPECT_EQ(out.inFlightTotal, 2u);
    EXPECT_EQ(out.entryCount, 4u);

    int failedCount = 0, inFlightCount = 0;
    bool sawA = false, sawB = false, sawC = false, sawD = false;
    for (uint32_t i = 0; i < out.entryCount; ++i) {
        const auto& e = out.entries[i];
        if (e.failed) {
            failedCount++;
            if (e.pathHash == 0xAAAA1111AAAA1111ull) { sawA = true; EXPECT_EQ(static_cast<int>(e.error), 1); }
            if (e.pathHash == 0xBBBB2222BBBB2222ull) { sawB = true; EXPECT_EQ(static_cast<int>(e.error), 3); }
        }
        if (e.inFlight) {
            inFlightCount++;
            if (e.pathHash == 0xCCCC3333CCCC3333ull) sawC = true;
            if (e.pathHash == 0xDDDD4444DDDD4444ull) sawD = true;
        }
    }
    EXPECT_EQ(failedCount, 2);
    EXPECT_EQ(inFlightCount, 2);
    EXPECT_TRUE(sawA && sawB && sawC && sawD);
}

TEST(ResourceLoaderSnapshot, EmptyLoaderReadsOk) {
    std::vector<uint8_t> buf(0x80, 0);
    BufferReader reader(buf.data(), buf.size());
    ResourceLoaderSnapshot out;
    ParseResourceLoader(buf.data(), reader, out);
    EXPECT_TRUE(out.readOk);
    EXPECT_EQ(out.failedTotal, 0u);
    EXPECT_EQ(out.inFlightTotal, 0u);
    EXPECT_EQ(out.entryCount, 0u);
}

TEST(ResourceLoaderSnapshot, NullLoaderNoData) {
    ResourceLoaderSnapshot out;
    BufferReader reader(nullptr, 0);
    ParseResourceLoader(nullptr, reader, out);
    EXPECT_FALSE(out.readOk);
    EXPECT_FALSE(out.hasData);
    EXPECT_EQ(out.entryCount, 0u);
}

TEST(ResourceLoaderSnapshot, SelfLoopNodeTerminates) {
    std::vector<uint8_t> buf(0x300, 0);
    uint8_t* base = buf.data();
    auto putU32 = [&](size_t off, uint32_t v) { std::memcpy(base + off, &v, sizeof(v)); };
    auto putU64 = [&](size_t off, uint64_t v) { std::memcpy(base + off, &v, sizeof(v)); };
    auto putPtr = [&](size_t off, size_t t) {
        uint64_t p = reinterpret_cast<uint64_t>(base + t);
        std::memcpy(base + off, &p, sizeof(p));
    };

    putU32(0x08, 1);        // map size
    putU32(0x0C, 4);        // capacity (bucket count)
    putPtr(0x00, 0x200);    // indexTable
    putPtr(0x10, 0x220);    // nodes
    putU32(0x18, 4);        // nodeList.capacity
    putU32(0x200, 0);
    putU32(0x204, 0xFFFFFFFF);
    putU32(0x208, 0xFFFFFFFF);
    putU32(0x20C, 0xFFFFFFFF);
    putU32(0x220, 0);                       // node 0 next = 0 (self-loop)
    putU64(0x228, 0xEEEE5555EEEE5555ull);

    BufferReader reader(base, buf.size());
    ResourceLoaderSnapshot out;
    ParseResourceLoader(base, reader, out);
    EXPECT_TRUE(out.readOk);
    EXPECT_EQ(out.entryCount, 1u);
    EXPECT_EQ(out.entries[0].pathHash, 0xEEEE5555EEEE5555ull);
    EXPECT_TRUE(out.entries[0].inFlight);
}
