#include "PreallocatedBuffer.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace redscope {

void PreallocatedBuffer::Reserve(size_t bytes) {
    if (data_) std::free(data_);
    data_ = static_cast<char*>(std::malloc(bytes));
    capacity_ = data_ ? bytes : 0;
    size_ = 0;
    overflow_ = data_ == nullptr;
}

void PreallocatedBuffer::Reset() noexcept {
    size_ = 0;
    overflow_ = false;
}

bool PreallocatedBuffer::Append(std::string_view s) noexcept {
    if (overflow_ || !data_) return false;
    if (size_ + s.size() > capacity_) { overflow_ = true; return false; }
    std::memcpy(data_ + size_, s.data(), s.size());
    size_ += s.size();
    return true;
}

bool PreallocatedBuffer::Append(const char* s) noexcept {
    if (!s) return false;
    return Append(std::string_view(s));
}

bool PreallocatedBuffer::AppendChar(char c) noexcept {
    if (overflow_ || !data_) return false;
    if (size_ + 1 > capacity_) { overflow_ = true; return false; }
    data_[size_++] = c;
    return true;
}

bool PreallocatedBuffer::Appendf(const char* fmt, ...) noexcept {
    if (overflow_ || !data_) return false;
    va_list ap; va_start(ap, fmt);
    size_t remaining = capacity_ - size_;
    int n = std::vsnprintf(data_ + size_, remaining, fmt, ap);
    va_end(ap);
    if (n < 0)                               { overflow_ = true; return false; }
    if (static_cast<size_t>(n) >= remaining) { size_ = capacity_; overflow_ = true; return false; }
    size_ += static_cast<size_t>(n);
    return true;
}

bool WriteBytesToFile(const wchar_t* path, const char* bytes, size_t len) noexcept {
    if (!path) return false;
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = (len == 0)
        ? TRUE
        : ::WriteFile(h, bytes, (DWORD)len, &written, nullptr);
    ::FlushFileBuffers(h);
    ::CloseHandle(h);
    return ok && written == len;
}

bool PreallocatedBuffer::WriteToFile(const wchar_t* path) const noexcept {
    return WriteBytesToFile(path, data_, size_);
}

PreallocatedBuffer& MainCrashBuffer() {
    static PreallocatedBuffer b;
    return b;
}

PreallocatedBuffer& SidecarBuffer() {
    static PreallocatedBuffer b;
    return b;
}

}
