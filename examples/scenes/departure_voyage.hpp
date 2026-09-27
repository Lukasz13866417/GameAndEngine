#pragma once
#include "key_batch.hpp"
#include <vector>

// The courier's voyage after it clears the Arabian skyway: a climb to an
// orbital gateway, a burn for the Moon, a low pass over its night side and a
// lunar base into sunrise, a run through a belt, the fleet's arrival, and
// the jump outward with the courier in formation.
//
// Each location is staged around the scene origin so close shots keep
// sub-centimetre 32-bit precision; bodies that appear in several locations
// jump to their next stage at the camera cuts between locations. The sun is
// placed along one fixed direction in every stage, as if infinitely far.
// Everything is ordinary instances and timeline keys, in kilometres.
namespace example::tunnel::voyage {
// What the skyway act hands over once the courier has cleared the terminal.
struct Handoff {
    vng::f32 time;
    vng::Vec3 position, velocity; // the courier, km and km/s
    vng::Vec3 earth_center;
    Look camera;                  // continuous with the skyway's last shot
};
struct Cast {
    vng::u32 hero, camera, earth;
    std::vector<vng::u32> traffic; // skyway traffic, hidden once the voyage leaves Earth
    editor_example::BlueprintId courier, transport, patrol, shuttle;
};
inline constexpr vng::f32 duration = 116;
// Camera cuts between locations: Earth orbit, the Moon, the belt.
inline constexpr vng::f32 moon_cut = 50.3F, belt_cut = 72.5F;
// Adds the voyage's bodies, installations, belt and fleet, and bakes the
// courier and camera from the handoff to the end into `keys`.
[[nodiscard]] vng::content::Result<void> author(editor_example::State&, KeyBatch& keys, const Handoff&, const Cast&);
// The authored extent: every instance's base and keyed positions, grown by
// its geometry's reach, so the world-bounds guide encloses Moon and sun.
[[nodiscard]] editor_example::WorldBounds bounds(const editor_example::State&);
} // namespace example::tunnel::voyage
