#pragma once
#include "../character/stage.hpp"
#include "../support/presentation.hpp"
#include "view.hpp"
#include <vng/gfx/camera.hpp>
#include <vng/opengl/readback.hpp>
#include <vng/rig_opengl/skinned_mesh_renderer.hpp>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace review {
// Where a character view looks from: degrees around him (0 in front, 90 his
// left), degrees above level, metres from the point looked at, and that
// point. The default shows all of him.
struct Orbit {
    vng::f32 yaw{30}, pitch{8}, distance{3.9F};
    vng::Vec3 target{0, .92F, 0};
    friend bool operator==(const Orbit&, const Orbit&) = default;
};
// The orbit after dragging the image by `drag` (normalized image units, x
// right and y down) in an image `aspect` wide: the point looked at slides
// with the pointer, as if the picture were grabbed.
[[nodiscard]] Orbit panned(const Orbit&, vng::Vec2 drag, vng::f32 aspect);
// The orbit `factor` times as far away, keeping what is under the normalized
// image point `at` where it is: zooming in toward it, or out from it.
[[nodiscard]] Orbit zoomed(const Orbit&, vng::f32 factor, vng::Vec2 at, vng::f32 aspect);

// A skinned character walking on the spot over a treadmill floor, seen from
// an orbit the reviewer turns. It plays one of the character's clips; a click
// names the bone that moves the skin there most and the point in the bind
// pose, so a note stays on the body as he moves. Clicks and markers are
// measured against the image last shown: its clip, time, orbit and extent.
class CharacterView final : public View {
public:
    // The CPU half of loading, safe on a worker thread.
    static vng::content::Result<character::Character> load(const std::filesystem::path&);
    static vng::resources::Result<std::unique_ptr<CharacterView>> create(vng::opengl::Device&, character::Character);

    [[nodiscard]] vng::f32 duration() const override;
    vng::resources::Result<void> update(vng::opengl::Device&, vng::f32 time, vng::Extent2D) override;
    [[nodiscard]] const std::shared_ptr<const vng::gfx::ImageData>& image() const override { return image_; }
    [[nodiscard]] vng::u64 revision() const override { return revision_; }
    [[nodiscard]] std::optional<vng::f32> shown_time() const override;
    [[nodiscard]] std::optional<Hit> pick(vng::Vec2 normalized) override;
    [[nodiscard]] std::optional<vng::Vec2> project(vng::Vec3 bind_point) const override;

    [[nodiscard]] std::vector<std::string> clips() const;
    [[nodiscard]] const std::string& clip() const { return clip_->name; }
    // False, changing nothing, when the character has no clip of that name.
    bool clip(std::string_view name);
    [[nodiscard]] const Orbit& orbit() const { return orbit_; }
    // Keeps the camera between straight below and straight above, 0.15 to 30 m
    // from a point within 10 m of him.
    void orbit(Orbit);

private:
    struct Frame {
        const character::Clip* clip;
        vng::f32 time;
        Orbit orbit;
        vng::Extent2D extent;
    };
    CharacterView(character::Character, character::Stage, example::DisplaySurface, vng::opengl::Rgba8ReadbackQueue);
    [[nodiscard]] vng::resources::Result<void> posed(const Frame&, vng::rig::Pose&) const;
    // The shown frame's skinning palette, computed on first use.
    [[nodiscard]] const std::vector<vng::Mat4>* shown_palette() const;

    character::Character character_;
    character::Stage stage_;
    example::DisplaySurface display_;
    vng::opengl::Rgba8ReadbackQueue readback_;
    std::optional<vng::opengl::SkinnedMeshRenderer> body_, floor_;
    vng::rig::Pose pose_, floor_pose_;
    const character::Clip* clip_{};
    Orbit orbit_;
    std::map<vng::u64, Frame> in_flight_;
    std::optional<Frame> shown_, requested_;
    std::shared_ptr<const vng::gfx::ImageData> image_;
    vng::u64 next_id_{1}, revision_{};
    mutable std::optional<std::vector<vng::Mat4>> palette_;
    mutable vng::u64 palette_revision_{};
};
} // namespace review
