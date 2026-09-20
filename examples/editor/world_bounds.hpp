#pragma once
#include "scene_coordinates.hpp"

namespace editor_example {
// Authored scene extent. Not a collision volume or an implicit clipping plane.
struct WorldBounds {
    vng::Vec3 minimum{-100, -100, -100}, maximum{100, 100, 100};
    friend bool operator==(const WorldBounds&, const WorldBounds&) = default;
};
inline constexpr vng::f32 world_min_extent = .01F;
inline bool valid_world_bounds(const WorldBounds& bounds) {
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(bounds.minimum[i]) || !std::isfinite(bounds.maximum[i]) ||
            bounds.minimum[i] < -scene_coordinate_limit || bounds.maximum[i] > scene_coordinate_limit ||
            bounds.maximum[i] - bounds.minimum[i] < world_min_extent) return false;
    return true;
}
}
