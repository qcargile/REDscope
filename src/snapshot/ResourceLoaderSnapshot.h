#pragma once
#include <cstdint>
#include <cstddef>

namespace redscope::snap {

struct ResourceLoaderSnapshot {
    static constexpr uint32_t kMaxEntries = 96;

    struct Entry {
        uint64_t pathHash = 0;
        bool     failed   = false;
        bool     inFlight = false;
        uint8_t  error    = 0;
    };

    bool     hasData       = false;
    bool     readOk        = false;
    uint32_t failedTotal   = 0;
    uint32_t inFlightTotal = 0;
    uint32_t entryCount    = 0;
    Entry    entries[kMaxEntries]{};
};

struct ResourceLoaderMemReader {
    virtual ~ResourceLoaderMemReader() = default;
    virtual bool Read(const void* addr, void* dst, size_t bytes) const noexcept = 0;
    virtual bool Readable(const void* addr, size_t bytes) const noexcept = 0;
};

void ParseResourceLoader(const void* loaderBase, const ResourceLoaderMemReader& mem,
                         ResourceLoaderSnapshot& out) noexcept;

void CaptureResourceLoaderSnapshot(ResourceLoaderSnapshot& out) noexcept;

}
