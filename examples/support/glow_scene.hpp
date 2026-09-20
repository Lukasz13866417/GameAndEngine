#pragma once

#include <array>
#include <cmath>
#include <numbers>

#include <vng/gfx/image.hpp>
#include <vng/render/mesh_renderer.hpp>

namespace example::glow {

using namespace vng;

inline void quad(
    render::SurfaceMesh& mesh,
    std::array<Vec3, 4> points,
    Vec4 color = {1, 1, 1, 1},
    f32 tiling = 1)
{
    const auto base = static_cast<u32>(mesh.vertex_count());
    const std::array uv{
        Vec2{0, 0},
        Vec2{tiling, 0},
        Vec2{tiling, tiling},
        Vec2{0, tiling},
    };
    for (std::size_t i = 0; i < 4; ++i) {
        auto& vertex = mesh.vertices().emplace_back();
        vertex.set(gfx::Position{}, points[i]);
        vertex.set(gfx::TexCoord<>{}, uv[i]);
        vertex.set(gfx::Color{}, color);
    }
    mesh.faces().emplace_back(base, base + 1, base + 2);
    mesh.faces().emplace_back(base, base + 2, base + 3);
}

inline render::SurfaceMesh ring()
{
    render::SurfaceMesh mesh(0);
    mesh.info().name = "Neon ring";

    constexpr u32 major = 96;
    constexpr u32 minor = 12;
    constexpr f32 tau = 2 * std::numbers::pi_v<f32>;

    // A thin torus gives the luminous ring a visible round cross-section.
    const auto point = [](f32 a, f32 b) {
        const auto radius = 1.0F + 0.045F * std::cos(b);
        return Vec3{
            radius * std::cos(a),
            radius * std::sin(a),
            0.045F * std::sin(b),
        };
    };
    for (u32 i = 0; i < major; ++i) {
        for (u32 j = 0; j < minor; ++j) {
            const auto a = tau * static_cast<f32>(i) / major;
            const auto b = tau * static_cast<f32>(j) / minor;
            const auto aa = tau * static_cast<f32>(i + 1) / major;
            const auto bb = tau * static_cast<f32>(j + 1) / minor;
            quad(mesh, {
                point(a, b),
                point(aa, b),
                point(aa, bb),
                point(a, bb),
            });
        }
    }
    return mesh;
}

inline render::SurfaceMesh box()
{
    render::SurfaceMesh mesh(0);
    mesh.info().name = "Display plinth";

    // Per-face colors keep the plinth readable with the unlit surface shader.
    quad(mesh, {{
        {-0.5F, -0.5F, 0.5F}, {0.5F, -0.5F, 0.5F},
        {0.5F, 0.5F, 0.5F}, {-0.5F, 0.5F, 0.5F},
    }}, {0.4F, 0.5F, 0.65F, 1});
    quad(mesh, {{
        {0.5F, -0.5F, -0.5F}, {-0.5F, -0.5F, -0.5F},
        {-0.5F, 0.5F, -0.5F}, {0.5F, 0.5F, -0.5F},
    }}, {0.25F, 0.32F, 0.45F, 1});
    quad(mesh, {{
        {0.5F, -0.5F, 0.5F}, {0.5F, -0.5F, -0.5F},
        {0.5F, 0.5F, -0.5F}, {0.5F, 0.5F, 0.5F},
    }}, {0.3F, 0.4F, 0.55F, 1});
    quad(mesh, {{
        {-0.5F, -0.5F, -0.5F}, {-0.5F, -0.5F, 0.5F},
        {-0.5F, 0.5F, 0.5F}, {-0.5F, 0.5F, -0.5F},
    }}, {0.35F, 0.42F, 0.6F, 1});
    quad(mesh, {{
        {-0.5F, 0.5F, 0.5F}, {0.5F, 0.5F, 0.5F},
        {0.5F, 0.5F, -0.5F}, {-0.5F, 0.5F, -0.5F},
    }}, {0.55F, 0.63F, 0.8F, 1});
    quad(mesh, {{
        {-0.5F, -0.5F, -0.5F}, {0.5F, -0.5F, -0.5F},
        {0.5F, -0.5F, 0.5F}, {-0.5F, -0.5F, 0.5F},
    }}, {0.2F, 0.25F, 0.35F, 1});
    return mesh;
}

inline render::SurfaceMesh floor()
{
    render::SurfaceMesh mesh(0);
    mesh.info().name = "Textured stage";
    quad(mesh, {{
        {-7, -1.3F, -5}, {-7, -1.3F, 5},
        {7, -1.3F, 5}, {7, -1.3F, -5},
    }}, {1, 1, 1, 1}, 12);
    return mesh;
}

inline gfx::ImageData grid_texture()
{
    gfx::ImageData image{
        .extent = {64, 64},
        .pixels = std::vector<std::byte>(64 * 64 * 4),
    };
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            const bool line = x < 2 || y < 2;
            const std::array<u8, 4> color = line
                ? std::array<u8, 4>{61, 85, 119, 255}
                : std::array<u8, 4>{21, 28, 44, 255};
            for (u32 channel = 0; channel < 4; ++channel) {
                image.pixels[(y * 64 + x) * 4 + channel] = std::byte{color[channel]};
            }
        }
    }
    return image;
}

inline Mat4 placement(
    Vec3 position,
    Vec3 scale = {1, 1, 1},
    f32 yaw = 0,
    f32 pitch = 0)
{
    const auto cy = std::cos(yaw);
    const auto sy = std::sin(yaw);
    const auto cx = std::cos(pitch);
    const auto sx = std::sin(pitch);

    // Column-major transform: scale, then pitch and yaw, then translate.
    Mat4 matrix{};
    matrix[0] = {cy * scale.x, 0, -sy * scale.x, 0};
    matrix[1] = {sy * sx * scale.y, cx * scale.y, cy * sx * scale.y, 0};
    matrix[2] = {sy * cx * scale.z, -sx * scale.z, cy * cx * scale.z, 0};
    matrix[3] = {position.x, position.y, position.z, 1};
    return matrix;
}

inline auto rings(f32 time)
{
    using Draw = render::SurfaceDraw;
    return std::array{
        Draw{
            .transform = placement({-2.6F, 0, 0}, {1, 1, 1}, time * 0.24F + 0.18F, 0.1F),
            .tint = {0.03F, 0.75F, 1, 1},
            .emission = 7,
        },
        Draw{
            .transform = placement({0, 0.2F, 0}, {1, 1, 1}, -time * 0.2F - 0.2F, 0.15F),
            .tint = {1, 0.21F, 0.015F, 1},
            .emission = 6,
        },
        Draw{
            .transform = placement({2.6F, 0, 0}, {1, 1, 1}, time * 0.3F - 0.3F, -0.12F),
            .tint = {0.5F, 0.025F, 1, 1},
            .emission = 8,
        },
        Draw{
            .transform = placement({0, 0.2F, 0}, {0.52F, 0.52F, 0.52F}, time * 0.33F + 0.8F, 0.75F),
            .tint = {1, 0.54F, 0.03F, 1},
            .emission = 4,
        },
    };
}

inline auto plinths()
{
    using Draw = render::SurfaceDraw;
    return std::array{
        Draw{
            .transform = placement({-2.6F, -1.16F, 0}, {2.4F, 0.25F, 1.6F}),
        },
        Draw{
            .transform = placement({0, -1.16F, 0}, {2.4F, 0.25F, 1.6F}),
        },
        Draw{
            .transform = placement({2.6F, -1.16F, 0}, {2.4F, 0.25F, 1.6F}),
        },
        Draw{
            .transform = placement({-2.6F, -1.005F, 0}, {1.75F, 0.025F, 0.045F}),
            .tint = {0.03F, 0.6F, 1, 1},
            .emission = 5,
        },
        Draw{
            .transform = placement({0, -1.005F, 0}, {1.75F, 0.025F, 0.045F}),
            .tint = {1, 0.21F, 0.02F, 1},
            .emission = 5,
        },
        Draw{
            .transform = placement({2.6F, -1.005F, 0}, {1.75F, 0.025F, 0.045F}),
            .tint = {0.6F, 0.02F, 1, 1},
            .emission = 5,
        },
    };
}

} // namespace example::glow
