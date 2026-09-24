#pragma once
#include "project.hpp"
#include "document_changes.hpp"
#include <vng/editor/inspector.hpp>
#include <utility>

namespace editor_example {
// The project defines its controls. The generic inspector UI adapter knows
// nothing about SunSettings/MeshSettings; callbacks only exist in the worker.
// Scene controls show evaluated values. Apply changes only edited properties:
// existing animation tracks get a key at the paused playhead; other values
// update instance defaults. Mesh blueprint inspection exposes no instance controls;
// isolated Sun effect inspection exposes appearance but no scene transform.
// Keyed like every other instance property: an animated value gets a key at
// the paused playhead, an unkeyed one updates the instance default. Whether
// the camera orbits its focus point is not animated; switching it rewrites
// the camera's position values so it does not move.
[[nodiscard]] vng::editor::Result<void> apply_lens(State&, DocumentChanges&, vng::u32 id,
    const CameraSettings& before, const CameraSettings& next);
// Make one camera the scene's camera from the playhead on, clearing every
// other camera that would otherwise be active there.
[[nodiscard]] vng::editor::Result<void> apply_active_camera(State&, DocumentChanges&, vng::u32 id);
class ProjectControls {
public:
    explicit ProjectControls(State& state) : state_(state) {}
    void describe_editor(vng::editor::Inspector&);
    // Callbacks commit typed property changes and publish exactly those targets.
    // Consumers must not infer a mesh change merely because a callback ran.
    [[nodiscard]] DocumentChanges take_changes() { return std::exchange(changes_, {}); }

private:
    State& state_;
    DocumentChanges changes_;
};
} // namespace editor_example
