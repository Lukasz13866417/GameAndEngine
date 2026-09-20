#pragma once
#include <vng/core/types.hpp>
#include <cmath>

namespace editor_example {
// File/IPC validation is independent of the viewport's adjustable tool policy.
inline constexpr vng::f32 min_instance_scale = .05F;
inline constexpr vng::f32 max_instance_scale = 1'000'000.F;
inline constexpr vng::f32 min_axis_scale = .05F;
inline constexpr vng::f32 max_axis_scale = 1'000'000.F;
struct ScaleLimits {
    vng::f32 instance{3};
    vng::f32 axis{20};
    vng::f32 factor{1000}; // Relative to the start of a mesh/region/S gesture.
    [[nodiscard]] bool valid() const {
        return std::isfinite(instance) && instance>=1 && instance<=max_instance_scale &&
            std::isfinite(axis) && axis>=1 && axis<=max_axis_scale &&
            std::isfinite(factor) && factor>=1 && factor<=max_instance_scale;
    }
};
}
