#pragma once
#include "../editor/runtime.hpp"
#include "../editor/selection.hpp"
#include "view.hpp"
#include <vng/gfx/image.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>

namespace review {
// One candidate's scene, its renderer, and the image it last presented.
// Clicks and note markers are measured against that image's own time,
// extent and camera, which may trail the playhead by a frame.
class CandidateView final : public View {
public:
    // The CPU half of loading, safe on a worker thread: reads the scene and
    // builds its picking indices so the first click does not stall.
    static vng::content::Result<editor_example::State> load_scene(const std::filesystem::path&);
    static vng::resources::Result<CandidateView> create(vng::opengl::Device&, editor_example::State);

    [[nodiscard]] vng::f32 duration() const override { return state_->document.timeline_duration; }
    vng::resources::Result<void> update(vng::opengl::Device&, vng::f32 time, vng::Extent2D) override;
    [[nodiscard]] const std::shared_ptr<const vng::gfx::ImageData>& image() const override { return image_; }
    [[nodiscard]] vng::u64 revision() const override { return revision_; }
    [[nodiscard]] std::optional<vng::f32> shown_time() const override;
    // The scene instance under the point, named as in the scene.
    [[nodiscard]] std::optional<Hit> pick(vng::Vec2 normalized) override;
    [[nodiscard]] std::optional<vng::Vec2> project(vng::Vec3) const override;

private:
    struct Frame {
        vng::f32 time;
        vng::Extent2D extent;
        vng::gfx::Camera camera;
    };
    CandidateView(std::unique_ptr<editor_example::State>, editor_example::Runtime);
    std::unique_ptr<editor_example::State> state_;
    editor_example::Runtime runtime_;
    std::map<vng::u64, Frame> in_flight_;
    std::optional<Frame> shown_, requested_;
    std::shared_ptr<const vng::gfx::ImageData> image_;
    vng::u64 next_id_{1}, revision_{};
};
} // namespace review
