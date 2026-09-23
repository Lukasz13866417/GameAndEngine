#pragma once

#include "animation.hpp"
#include <vector>

namespace editor_example {
// Scene files before editor_project 5 had no camera instances. They stored the
// simulation camera as one document-level orbit pose ("animation_camera"),
// animated by yaw/pitch/distance/target/zoom tracks on a reserved timeline
// object. This is the only code that still knows that representation: the
// decoder collects the old shot, and loading continues with an ordinary
// camera instance in its place.
inline constexpr vng::u64 legacy_camera_object = vng::u64{1} << 32;

struct LegacyCameraShot {
    CameraPose pose;
    std::vector<vng::timeline::Track> tracks; // all on legacy_camera_object
};

// The typed legacy camera properties, so the file reader can parse their keys.
[[nodiscard]] std::vector<AnimationProperty> legacy_camera_properties(const CameraPose& base);

// Adds an active "Animation camera" placed at the shot and keyed at every
// legacy key time, holding where all legacy keys at that time held. A scene
// that already has camera instances never used the shot, so it is dropped.
[[nodiscard]] vng::content::Result<void> migrate_legacy_camera(State&, const LegacyCameraShot&);
} // namespace editor_example
