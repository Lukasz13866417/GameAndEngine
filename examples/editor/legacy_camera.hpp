#pragma once

#include "animation.hpp"
#include <vng/content/document.hpp>
#include <vector>

namespace editor_example {
// Scene files before editor_project 5 also had a document-level "shot": one
// orbit pose ("animation_camera", or in the oldest files the `view` camera),
// animated by yaw/pitch/distance/target/zoom tracks on a reserved timeline
// object. It was the simulation camera whenever a scene had no camera
// instances. This component is the only code that knows that shape: the
// decoder asks it to read the shot and route its tracks, then to turn it into
// an ordinary camera instance.
inline constexpr vng::u64 legacy_camera_object = vng::u64{1} << 32;

struct LegacyCameraShot {
    CameraPose pose;
    std::vector<vng::timeline::Track> tracks; // all on legacy_camera_object
};

// The shot's pose in a pre-version-5 file: `animation_camera` when present,
// otherwise the file's `view` camera, which the caller has already read.
[[nodiscard]] LegacyCameraShot read_legacy_camera_shot(vng::content::Reader file, const CameraPose& view_camera);
// The typed legacy camera properties, so the file reader can parse their keys.
[[nodiscard]] std::vector<AnimationProperty> legacy_camera_properties(const CameraPose& base);

// Adds an active "Animation camera" that follows the shot, placed where the
// shot's eye was (clamped to the scene's coordinate range if it lay beyond). Each new track comes
// only from the old tracks it depends on: focus and zoom copy distance and zoom
// key for key; rotation takes yaw and pitch key times; the eye, which moved on
// an orbit, is refined between those times to within about half a pixel. A cut
// in one component while another still moves becomes a key a millisecond
// before the cut plus a held key at it. Only if the result would exceed a
// timeline key limit are the least significant keys dropped, so every old
// scene loads. A scene that already has camera instances never used the shot,
// so it is dropped; so is the shot of a scene already at the instance limit.
[[nodiscard]] vng::content::Result<void> migrate_legacy_camera(State&, const LegacyCameraShot&);
} // namespace editor_example
