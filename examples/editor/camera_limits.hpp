#pragma once
#include <vng/core/types.hpp>
#include <cmath>

namespace editor_example {
// Multipliers of the distance-aware drag response; rotation is degrees/pixel.
struct CameraDragSpeeds {
    float pan{1}, forward{1}, rotation{.3F};
    friend bool operator==(const CameraDragSpeeds&, const CameraDragSpeeds&) = default;
};
struct WalkSpeeds {
    float forward{10}, sideways{10}, vertical{10}, fast_multiplier{4};
    friend bool operator==(const WalkSpeeds&, const WalkSpeeds&) = default;
};
// Serialization/numerical limits, not the user's navigation preferences.
inline constexpr vng::f32 camera_min_distance = .001F;
inline constexpr vng::f32 camera_max_distance = 1'000'000.F;
inline constexpr vng::f32 camera_min_zoom = .05F;
inline constexpr vng::f32 camera_max_zoom = 1000.F;
struct OrbitDistanceRange {
    vng::f32 minimum{.01F}, maximum{10'000.F};
    friend bool operator==(const OrbitDistanceRange&, const OrbitDistanceRange&) = default;
};
inline bool valid_orbit_distance_range(OrbitDistanceRange range) {
    return std::isfinite(range.minimum) && std::isfinite(range.maximum) &&
        range.minimum >= camera_min_distance && range.maximum <= camera_max_distance &&
        range.minimum < range.maximum;
}
}
