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
