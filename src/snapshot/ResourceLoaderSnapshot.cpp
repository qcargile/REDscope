#include "ResourceLoaderSnapshot.h"
#include "../util/SehGuardedRead.h"

#ifdef REDSCOPE_USE_RTTI
#include <RED4ext/ResourceLoader.hpp>
#endif

namespace redscope::snap {

namespace {

constexpr uintptr_t kFailedEntriesOff  = 0x30;
constexpr uintptr_t kFailedSizeOff     = 0x3C;
constexpr uintptr_t kTokensIndexTblOff = 0x00;
constexpr uintptr_t kTokensSizeOff     = 0x08;
constexpr uintptr_t kTokensCapacityOff = 0x0C;
constexpr uintptr_t kTokensNodesOff    = 0x10;
constexpr uintptr_t kTokensNodeCapOff  = 0x18;
constexpr uintptr_t kNodeStride        = 0x20;
constexpr uintptr_t kNodeNextOff       = 0x00;
constexpr uintptr_t kNodeKeyOff        = 0x08;
constexpr uintptr_t kSharedPtrStride   = 0x10;
constexpr uintptr_t kTokenPathOff      = 0x48;
constexpr uintptr_t kTokenErrorOff     = 0x5C;
constexpr uintptr_t kLoaderSize        = 0x80;
constexpr uintptr_t kTokenSize         = 0x60;

constexpr uint32_t kMaxFailedScan   = 96;
constexpr uint32_t kMaxBuckets      = 65536;
constexpr uint32_t kInFlightHardCap = 8192;
constexpr uint32_t kInvalidIndex    = 0xFFFFFFFF;

template <typename T>
bool ReadVal(const ResourceLoaderMemReader& mem, const void* addr, T& out) noexcept {
    return mem.Read(addr, &out, sizeof(T));
}

bool PushEntry(ResourceLoaderSnapshot& out, uint64_t pathHash, bool failed, bool inFlight, uint8_t error) noexcept {
    if (out.entryCount >= ResourceLoaderSnapshot::kMaxEntries) return false;
    ResourceLoaderSnapshot::Entry& e = out.entries[out.entryCount++];
    e.pathHash = pathHash;
    e.failed   = failed;
    e.inFlight = inFlight;
    e.error    = error;
    return true;
}

}

void ParseResourceLoader(const void* loaderBase, const ResourceLoaderMemReader& mem,
                         ResourceLoaderSnapshot& out) noexcept {
    out = ResourceLoaderSnapshot{};
    if (!loaderBase || !mem.Readable(loaderBase, kLoaderSize)) return;
    const auto* lp = reinterpret_cast<const uint8_t*>(loaderBase);
    out.readOk = true;

    const uint8_t* failedArrayBase = nullptr;
    uint32_t failedSize = 0;
    ReadVal(mem, lp + kFailedEntriesOff, failedArrayBase);
    ReadVal(mem, lp + kFailedSizeOff, failedSize);
    out.failedTotal = failedSize;
    if (failedArrayBase && failedSize > 0) {
        uint32_t n = failedSize < kMaxFailedScan ? failedSize : kMaxFailedScan;
        if (mem.Readable(failedArrayBase, static_cast<size_t>(n) * kSharedPtrStride)) {
            for (uint32_t i = 0; i < n; ++i) {
                const uint8_t* tokenPtr = nullptr;
                ReadVal(mem, failedArrayBase + static_cast<size_t>(i) * kSharedPtrStride, tokenPtr);
                if (!tokenPtr || !mem.Readable(tokenPtr, kTokenSize)) continue;
                uint64_t pathHash = 0;
                uint8_t error = 0;
                ReadVal(mem, tokenPtr + kTokenPathOff, pathHash);
                ReadVal(mem, tokenPtr + kTokenErrorOff, error);
                if (pathHash != 0) PushEntry(out, pathHash, true, false, error);
            }
        }
    }

    uint32_t mapSize = 0;
    ReadVal(mem, lp + kTokensSizeOff, mapSize);
    out.inFlightTotal = mapSize;
    if (mapSize > 0) {
        const uint8_t* indexTable = nullptr;
        const uint8_t* nodes = nullptr;
        uint32_t bucketCount = 0;
        uint32_t nodeCapacity = 0;
        ReadVal(mem, lp + kTokensIndexTblOff, indexTable);
        ReadVal(mem, lp + kTokensCapacityOff, bucketCount);
        ReadVal(mem, lp + kTokensNodesOff, nodes);
        ReadVal(mem, lp + kTokensNodeCapOff, nodeCapacity);
        if (indexTable && nodes && bucketCount > 0 && bucketCount <= kMaxBuckets &&
            nodeCapacity > 0 && nodeCapacity <= kMaxBuckets &&
            mem.Readable(indexTable, static_cast<size_t>(bucketCount) * sizeof(uint32_t))) {
            const bool nodesBulkOk = mem.Readable(nodes, static_cast<size_t>(nodeCapacity) * kNodeStride);
            uint32_t iters = 0;
            for (uint32_t b = 0; b < bucketCount && iters < kInFlightHardCap &&
                                 out.entryCount < ResourceLoaderSnapshot::kMaxEntries; ++b) {
                uint32_t idx = kInvalidIndex;
                ReadVal(mem, indexTable + static_cast<size_t>(b) * sizeof(uint32_t), idx);
                uint32_t chainLen = 0;
                while (idx != kInvalidIndex && idx < nodeCapacity && chainLen < nodeCapacity &&
                       iters < kInFlightHardCap &&
                       out.entryCount < ResourceLoaderSnapshot::kMaxEntries) {
                    ++iters;
                    ++chainLen;
                    const uint8_t* node = nodes + static_cast<size_t>(idx) * kNodeStride;
                    if (!nodesBulkOk && !mem.Readable(node, kNodeStride)) break;
                    uint64_t key = 0;
                    uint32_t next = kInvalidIndex;
                    ReadVal(mem, node + kNodeKeyOff, key);
                    ReadVal(mem, node + kNodeNextOff, next);
                    if (key != 0) PushEntry(out, key, false, true, 0);
                    if (next == idx) break;
                    idx = next;
                }
            }
        }
    }

    out.hasData = out.readOk;
}

#ifdef REDSCOPE_USE_RTTI
namespace {
struct LiveMemReader : ResourceLoaderMemReader {
    bool Read(const void* addr, void* dst, size_t bytes) const noexcept override {
        return redscope::SehSafeRead(dst, addr, bytes);
    }
    bool Readable(const void* addr, size_t bytes) const noexcept override {
        return redscope::LooksReadable(addr, bytes);
    }
};
}

void CaptureResourceLoaderSnapshot(ResourceLoaderSnapshot& out) noexcept {
    out = ResourceLoaderSnapshot{};
    RED4ext::ResourceLoader* loader = RED4ext::ResourceLoader::Get();
    if (!loader) return;
    LiveMemReader mem;
    ParseResourceLoader(loader, mem, out);
}
#else
void CaptureResourceLoaderSnapshot(ResourceLoaderSnapshot& out) noexcept {
    out = ResourceLoaderSnapshot{};
}
#endif

}
