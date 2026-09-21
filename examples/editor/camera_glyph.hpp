#pragma once

#include "project.hpp"
#include "rotation_math.hpp"
#include <array>
#include <cmath>
#include <numbers>

namespace editor_example {
// A scene camera's preview glyph and its pick volume come from one
// description, so the thing the user sees is the thing the click hits. Sizes
// follow the focus distance: a camera focused far away is framed from far
// away, and its body stays legible there.
struct CameraGlyph {
    vng::Vec3 eye, forward, up, right;
    vng::f32 depth{}, half_height{}, half_width{}, body{}; // frustum depth, rim half extents, body length
    [[nodiscard]] vng::Vec3 at(vng::f32 x, vng::f32 y, vng::f32 z) const {
        return {eye.x + forward.x * z + right.x * x + up.x * y, eye.y + forward.y * z + right.y * x + up.y * y,
                eye.z + forward.z * z + right.z * x + up.z * y};
    }
    // The box body sits behind the lens; a sphere around it is the pick target.
    [[nodiscard]] vng::Vec3 body_center() const { return at(0, .12F * body, -.5F * body); }
    [[nodiscard]] vng::f32 pick_radius() const { return .8F * body; }
};
// Body box (12), lens ring (8), two film reels (16), frustum edges (4), rim (4), up tick (2), look line (1).
inline constexpr std::size_t camera_glyph_line_count = 47;

[[nodiscard]] inline CameraGlyph camera_glyph(const SceneInstance& evaluated) {
    using namespace vng;
    CameraGlyph glyph;
    const auto* lens = std::get_if<CameraSettings>(&evaluated.settings);
    const CameraSettings settings = lens ? *lens : CameraSettings{};
    glyph.eye = evaluated.transform.position;
    glyph.forward = rotation_math::direction(evaluated.transform.rotation, {0, 0, -1});
    glyph.up = rotation_math::direction(evaluated.transform.rotation, {0, 1, 0});
    glyph.right = rotation_math::direction(evaluated.transform.rotation, {1, 0, 0});
    glyph.depth = std::clamp(settings.focus * .15F, .35F, 12.F);
    glyph.half_height = glyph.depth * std::tan(camera_vertical_fov * .5F * std::numbers::pi_v<f32> / 180) /
                        std::max(settings.zoom, camera_min_zoom);
    glyph.half_width = glyph.half_height * 16 / 9;
    glyph.body = glyph.depth * .4F;
    return glyph;
}
// Body box (12), lens disc (8), two reels with both faces (32).
inline constexpr std::size_t camera_glyph_triangle_count = 52;

// Emits every glyph segment as emit(from, to). Colors and instance identity
// belong to the caller.
template<class Emit>
void camera_glyph_lines(const CameraGlyph& g, Emit emit) {
    using namespace vng;
    const auto b = g.body;
    // Body: a box from the lens plane back along the look direction.
    std::array<Vec3, 8> box;
    for (unsigned i = 0; i < 8; ++i)
        box[i] = g.at(i & 1 ? .35F * b : -.35F * b, i & 2 ? .25F * b : -.25F * b, i & 4 ? -b : 0.F);
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned c = 0; c < 3; ++c)
            if (!(i & (1U << c))) emit(box[i], box[i | (1U << c)]);
    // Lens ring on the front face.
    const auto ring = [&](Vec3 center, f32 radius, unsigned segments) {
        Vec3 previous{};
        for (unsigned i = 0; i <= segments; ++i) {
            const auto angle = static_cast<f32>(i) * 2 * std::numbers::pi_v<f32> / static_cast<f32>(segments);
            const Vec3 point{center.x + (g.right.x * std::cos(angle) + g.up.x * std::sin(angle)) * radius,
                             center.y + (g.right.y * std::cos(angle) + g.up.y * std::sin(angle)) * radius,
                             center.z + (g.right.z * std::cos(angle) + g.up.z * std::sin(angle)) * radius};
            if (i) emit(previous, point);
            previous = point;
        }
    };
    ring(g.at(0, 0, .01F * b), .18F * b, 8);
    // Two film reels on top make it read as a camera from any angle.
    ring(g.at(-.28F * b, .47F * b, -.5F * b), .2F * b, 8);
    ring(g.at(.28F * b, .47F * b, -.5F * b), .2F * b, 8);
    // Frustum: edges from the eye to the rim, the rim, an up tick and the look line.
    const std::array corners{g.at(-g.half_width, -g.half_height, g.depth), g.at(g.half_width, -g.half_height, g.depth),
                             g.at(g.half_width, g.half_height, g.depth), g.at(-g.half_width, g.half_height, g.depth)};
    for (const auto& corner : corners) emit(g.eye, corner);
    for (std::size_t i = 0; i < 4; ++i) emit(corners[i], corners[(i + 1) % 4]);
    const auto peak = g.at(0, g.half_height * 1.6F, g.depth);
    emit(corners[2], peak);
    emit(peak, corners[3]);
    emit(g.at(0, 0, g.depth), g.at(0, 0, std::max(g.depth * 2, g.depth / .3F)));
}

// The solid parts of the glyph as emit(a, b, c) triangles: the box body, the
// lens disc and both faces of each reel. The frustum stays a wire "screen".
template<class Emit>
void camera_glyph_triangles(const CameraGlyph& g, Emit emit) {
    using namespace vng;
    const auto b = g.body;
    std::array<Vec3, 8> box;
    for (unsigned i = 0; i < 8; ++i)
        box[i] = g.at(i & 1 ? .35F * b : -.35F * b, i & 2 ? .25F * b : -.25F * b, i & 4 ? -b : 0.F);
    // Each face: the four corners sharing one fixed bit, split into two triangles.
    for (unsigned axis = 0; axis < 3; ++axis)
        for (unsigned side = 0; side < 2; ++side) {
            const unsigned fixed = side << axis, u = 1U << ((axis + 1) % 3), v = 1U << ((axis + 2) % 3);
            emit(box[fixed], box[fixed | u], box[fixed | u | v]);
            emit(box[fixed], box[fixed | u | v], box[fixed | v]);
        }
    const auto disc = [&](Vec3 center, f32 radius, unsigned segments) {
        const auto rim = [&](unsigned i) {
            const auto angle = static_cast<f32>(i % segments) * 2 * std::numbers::pi_v<f32> / static_cast<f32>(segments);
            return Vec3{center.x + (g.right.x * std::cos(angle) + g.up.x * std::sin(angle)) * radius,
                        center.y + (g.right.y * std::cos(angle) + g.up.y * std::sin(angle)) * radius,
                        center.z + (g.right.z * std::cos(angle) + g.up.z * std::sin(angle)) * radius};
        };
        for (unsigned i = 0; i < segments; ++i) emit(center, rim(i), rim(i + 1));
    };
    disc(g.at(0, 0, .01F * b), .18F * b, 8);
    for (const auto x : {-.28F * b, .28F * b}) {
        disc(g.at(x, .47F * b, -.44F * b), .2F * b, 8);
        disc(g.at(x, .47F * b, -.56F * b), .2F * b, 8);
    }
}
} // namespace editor_example
