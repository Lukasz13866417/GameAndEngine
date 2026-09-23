#pragma once

#include "animation.hpp"
#include <vector>

namespace editor_example {
// The view independent Play shows. It looks through the scene's active camera
// until the viewer navigates in the Play window, and any authored change to a
// scene camera, at whatever time it is keyed, hands the view back to it. A
// scene without a camera plays from the view Play started with (the editor's)
// and keeps its current view if its camera is removed; it never follows later
// editor navigation. Owned by the preview worker; no window or GPU state.
class PlayCamera {
public:
    // The authored camera data a Play view depends on: every camera instance
    // and its timeline tracks. Compare two to detect an authored camera change.
    struct Cameras {
        std::vector<SceneInstance> instances;
        std::vector<vng::timeline::Track> tracks;
        friend bool operator==(const Cameras&, const Cameras&) = default;
    };
    [[nodiscard]] static Cameras cameras(const State& state) {
        Cameras result;
        for (const auto& instance : state.document.instances)
            if (std::holds_alternative<CameraSettings>(instance.settings)) result.instances.push_back(instance);
        for (const auto& track : state.document.timeline.tracks())
            if (track.target.object <= UINT32_MAX && is_camera_instance(state, static_cast<vng::u32>(track.target.object)))
                result.tracks.push_back(track);
        return result;
    }

    void start(const State& state, vng::f32 time) {
        const auto scene = evaluate_camera(state, time);
        view_ = scene.value_or(state.viewport.editor_camera);
        held_ = !scene;
    }
    // The pose to render at `time`.
    [[nodiscard]] const CameraPose& view(const State& state, vng::f32 time) {
        if (!held_)
            if (const auto scene = evaluate_camera(state, time)) view_ = *scene;
        return view_;
    }
    // The viewer moved the Play camera: hold that view.
    void navigated(const CameraPose& pose) { view_ = pose; held_ = true; }
    // Look through the scene camera again, if there is one.
    void follow(const State& state) { held_ = !has_camera(state); }
    // After an authored change: true when the scene's camera data changed and
    // the view was handed back to it.
    bool authored(const Cameras& before, const State& after) {
        if (cameras(after) == before) return false;
        follow(after);
        return true;
    }
    [[nodiscard]] bool held() const noexcept { return held_; }

private:
    CameraPose view_{};
    bool held_{};
};
} // namespace editor_example
