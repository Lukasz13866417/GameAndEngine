#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <vector>

#include <vng/gfx/geometry.hpp>
#include <vng/gfx/mesh.hpp>
#include <vng/rig/diagnostic.hpp>

namespace example {

inline int fail(const vng::rig::Diagnostic& diagnostic)
{
    std::cerr << diagnostic.message << '\n';
    return 1;
}

namespace rigging {

using Vertex = vng::gfx::Record<
    vng::gfx::Position, vng::gfx::Normal, vng::gfx::Color>;

struct Tube final {
    vng::gfx::Mesh<Vertex> mesh;
    std::vector<std::array<vng::f32, 3>> weights;
};

// Ordinary authoring data: an upright tube, three smooth influence bands, and
// duplicated cap vertices for hard cap normals. Nothing here knows a backend.
inline Tube make_tube()
{
    using namespace vng;
    constexpr u32 sides = 20;
    constexpr u32 segments = 48;
    constexpr f32 height = 3.0F;
    constexpr f32 radius = 0.28F;
    constexpr u32 side_vertices = (segments + 1) * sides;
    constexpr u32 cap_vertices = sides + 1;
    constexpr u32 vertex_count = side_vertices + 2 * cap_vertices;
    Tube tube{gfx::Mesh<Vertex>{vertex_count}, {}};
    tube.mesh.info().name = "three-bone tube";
    tube.weights.resize(vertex_count);

    const auto smooth = [](f32 x) {
        x = std::clamp(x, 0.0F, 1.0F);
        return x * x * (3.0F - 2.0F * x);
    };
    const auto vertex = [&](u32 index, Vec3 position, Vec3 normal) {
        const auto t = position.y / height;
        auto& value = tube.mesh.vertices()[index];
        value.set(gfx::Position{}, position);
        value.set(gfx::Normal{}, normal);
        value.set(gfx::Color{}, {0.12F + 0.88F * t, 0.55F - 0.15F * t,
                                0.95F - 0.85F * t, 1.0F});
        const auto middle = smooth((position.y - 0.55F) / 0.9F);
        const auto tip = smooth((position.y - 1.55F) / 0.9F);
        tube.weights[index] = {1.0F - middle, middle - tip, tip};
    };
    const auto radial = [](u32 side) {
        const auto angle = 2.0F * std::numbers::pi_v<f32> * static_cast<f32>(side) / sides;
        return Vec3{std::cos(angle), 0.0F, std::sin(angle)};
    };
    for (u32 ring = 0; ring <= segments; ++ring) {
        const auto y = height * static_cast<f32>(ring) / segments;
        for (u32 side = 0; side < sides; ++side) {
            const auto normal = radial(side);
            vertex(ring * sides + side, {radius * normal.x, y, radius * normal.z}, normal);
        }
    }
    for (u32 ring = 0; ring < segments; ++ring) {
        for (u32 side = 0; side < sides; ++side) {
            const auto next = (side + 1) % sides;
            const auto lower = ring * sides;
            const auto upper = lower + sides;
            tube.mesh.faces().emplace_back(lower + side, upper + side, lower + next);
            tube.mesh.faces().emplace_back(lower + next, upper + side, upper + next);
        }
    }
    for (u32 cap = 0; cap < 2; ++cap) {
        const auto start = side_vertices + cap * cap_vertices;
        const auto center = start + sides;
        const auto y = height * static_cast<f32>(cap);
        const Vec3 normal{0, cap == 0 ? -1.0F : 1.0F, 0};
        vertex(center, {0, y, 0}, normal);
        for (u32 side = 0; side < sides; ++side) {
            const auto point = radial(side);
            vertex(start + side, {radius * point.x, y, radius * point.z}, normal);
            const auto next = (side + 1) % sides;
            if (cap == 0) tube.mesh.faces().emplace_back(center, start + side, start + next);
            else tube.mesh.faces().emplace_back(center, start + next, start + side);
        }
    }
    return tube;
}

} // namespace rigging
} // namespace example
