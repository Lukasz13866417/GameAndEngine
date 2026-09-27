#pragma once
#include <vng/core/types.hpp>

namespace editor_example {
// Authored presentation values, not window/performance preferences. Old scenes
// keep their existing appearance; cinematic scenes opt into the star field.
struct EnvironmentSettings {
    vng::u32 stars{}, star_seed{32};
    vng::f32 exposure{.9F}, bloom_threshold{6.5F}, bloom_strength{};
    // How far the standalone scene camera sees. Zero keeps the orbit rule (four
    // times the camera focus); the editor uses its viewing-distance preference.
    vng::f32 view_distance{};
    friend bool operator==(const EnvironmentSettings&, const EnvironmentSettings&) = default;
};
} // namespace editor_example
