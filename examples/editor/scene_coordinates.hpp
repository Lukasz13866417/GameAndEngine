#pragma once
#include <vng/core/types.hpp>
#include <cmath>

namespace editor_example {
// Shared numerical/serialization safety envelope, not an authored world border.
// Instance placement, camera targets, animation and transport must agree on it.
inline constexpr vng::f32 scene_coordinate_limit = 1'000'000.F;
inline bool valid_scene_position(vng::Vec3 position) {
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!std::isfinite(position[axis]) || std::abs(position[axis]) > scene_coordinate_limit)
            return false;
    return true;
}
}
