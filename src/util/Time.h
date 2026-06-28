#pragma once
#include <chrono>
#include <cstdint>

namespace redscope {

inline int64_t NowNs() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

}
