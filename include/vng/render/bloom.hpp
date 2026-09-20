#pragma once

#include <vng/core/types.hpp>

namespace vng::render {

// Input colors and the threshold are linear HDR values. Exposure and strength
// are ordinary per-frame effect state; changing them never rebuilds resources.
struct BloomSettings final {
    f32 threshold{1.0F};
    f32 strength{0.25F};
    f32 exposure{1.0F};
};

struct BloomOptions final {
    u32 levels{5}; // [1, 8], truncated when the pyramid reaches 1x1
};

struct MakeBloomBuilder final {
    template<class Device>
    [[nodiscard]] auto operator()(Device& device) const
        -> decltype(make_backend_bloom_builder(device))
    {
        return make_backend_bloom_builder(device);
    }
};
inline constexpr MakeBloomBuilder bloom_builder{};

} // namespace vng::render
