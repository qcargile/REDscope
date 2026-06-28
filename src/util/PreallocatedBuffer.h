#pragma once
#include <cstddef>
#include <string_view>

namespace redscope {

class PreallocatedBuffer {
public:
    void Reserve(size_t bytes);
    void Reset() noexcept;

    bool Append(std::string_view s) noexcept;
    bool Append(const char* s) noexcept;
    bool Appendf(const char* fmt, ...) noexcept;
    bool AppendChar(char c) noexcept;

    bool Overflowed() const noexcept { return overflow_; }
    size_t Size() const noexcept { return size_; }
    size_t Capacity() const noexcept { return capacity_; }
    const char* Data() const noexcept { return data_; }

    bool WriteToFile(const wchar_t* path) const noexcept;

private:
    char*  data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    bool   overflow_ = false;
};

PreallocatedBuffer& MainCrashBuffer();
PreallocatedBuffer& SidecarBuffer();

bool WriteBytesToFile(const wchar_t* path, const char* bytes, size_t len) noexcept;

}
