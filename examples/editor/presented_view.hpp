#pragma once
#include "project.hpp"
#include <vng/editor/preview.hpp>

namespace editor_example {
// Pixels and their view metadata form one immutable observation. Neither the
// UI's newest camera target nor the worker's next request describes these pixels.
inline CameraPose presented_pose(const vng::editor::preview::FrameInfo& info) {
    const auto& p=info.view_camera;
    return {p[0],p[1],p[2],{p[3],p[4],p[5]},p[6]};
}
inline vng::gfx::Camera presented_camera(const vng::editor::preview::FrameInfo& info) {
    return camera(presented_pose(info),static_cast<ViewMode>(info.view_mode), info.view_far_plane);
}
inline bool matches_view(const vng::editor::preview::FrameInfo& info, const State& state) {
    return info.has_view && info.view_mode==static_cast<vng::u32>(state.viewport.mode) &&
        (state.viewport.mode!=ViewMode::mesh || info.inspected_mesh==static_cast<vng::u32>(state.viewport.inspected_mesh));
}
} // namespace editor_example
