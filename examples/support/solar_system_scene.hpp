#pragma once
#include "../editor/project.hpp"

// One editable establishing shot: the Kestrel leaves Earth toward a distant
// asteroid belt where the fleet waits. Everything is an ordinary scene
// instance; the sun is the scene's light. Only the starting keyframe exists,
// so the rest of the shot is authored in the editor.
namespace example::solar_system {
inline constexpr vng::f32 duration=40;
// Identities: the fleet scene supplies 1..18, then Earth, six distant wings and the belt.
inline constexpr vng::u32 hero=1, sun=2, first_ship=3, formation_count=16, earth=19,
    first_wing=20, wing_count=24, first_rock=44, rock_count=600;
inline constexpr vng::u32 ship_count=formation_count+wing_count;
inline constexpr auto earth_blueprint=static_cast<editor_example::BlueprintId>(6);
inline constexpr auto first_rock_blueprint=static_cast<editor_example::BlueprintId>(7);
// Layout along the view axis from the eye: Earth beside the camera, the fleet
// seven tenths of the way to the belt, so the ships sit ~30% of the belt
// distance in front of the rocks.
inline constexpr vng::f32 earth_radius=12, fleet_distance=210, belt_distance=300;
inline constexpr vng::Vec3 earth_center{0,0,0}, eye{20,3,8};
[[nodiscard]] vng::Vec3 view_direction();
[[nodiscard]] vng::Vec3 sun_position();
inline bool is_rock(const editor_example::SceneInstance& instance) {
    const auto blueprint=static_cast<vng::u32>(instance.blueprint);
    return blueprint>=static_cast<vng::u32>(first_rock_blueprint) &&
           blueprint<static_cast<vng::u32>(first_rock_blueprint)+3;
}
[[nodiscard]] vng::content::Result<editor_example::State>
author_scene(const std::filesystem::path& asset_directory);
}
