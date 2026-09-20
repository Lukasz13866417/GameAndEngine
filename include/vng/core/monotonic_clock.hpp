#pragma once
#include <vng/core/types.hpp>
#include <time.h>

namespace vng {
// One Linux CLOCK_MONOTONIC epoch across processes. Zero means unavailable.
// Wall clocks and GPU clocks must not be subtracted from these timestamps.
[[nodiscard]] inline u64 monotonic_ns() noexcept {
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<u64>(value.tv_sec) * 1'000'000'000ULL + static_cast<u64>(value.tv_nsec);
}
}
