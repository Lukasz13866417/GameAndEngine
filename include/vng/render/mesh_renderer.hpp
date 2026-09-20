#pragma once

#include <cmath>
#include <memory>
#include <vng/gfx/geometry.hpp>
#include <vng/gfx/mesh.hpp>
#include <vng/render/graphics_state.hpp>
#include <vng/resources/resources.hpp>

namespace vng::render {

// The standard textured mesh renderer consumes logical values, independently
// of authored stream order/packing. Keep CPU topology for inspection and reload.
using SurfaceVertex = gfx::Record<gfx::Position, gfx::TexCoord<>, gfx::Color>;
using SurfaceMesh = gfx::Mesh<SurfaceVertex>;
using MeshSource = std::shared_ptr<const SurfaceMesh>;

// The shader contract for one SurfaceDraw. These are divisor-one attributes,
// not uniforms: custom surface shaders can keep distinct values in one draw.
namespace surface {
template<unsigned Column> requires (Column < 4)
struct ModelColumn : gfx::Semantic<Vec4> {};
struct Tint : gfx::Semantic<Vec4> {};
struct Emission : gfx::Semantic<f32> {};
using Instance = gfx::Record<ModelColumn<0>, ModelColumn<1>, ModelColumn<2>, ModelColumn<3>, Tint, Emission>;
}

template<class Position = gfx::Position, class UV = gfx::TexCoord<>, class Color = gfx::Color>
struct MeshFields { Position position{}; UV uv{}; Color color{}; };

struct SurfaceDraw {
    Mat4 transform{Mat4::identity()};
    Vec4 tint{1, 1, 1, 1};
    f32 emission{}; // extra linear radiance, not an alpha or bloom setting
    bool depth_test{true};
    bool depth_write{true};
    CullMode cull{CullMode::back};
};

struct SurfaceRenderStats {
    std::size_t instances{}, draw_calls{};
};

struct MeshRendererBuilderFactory {
    template<class Device>
    auto operator()(Device& device) const -> decltype(make_backend_mesh_renderer_builder(device))
    { return make_backend_mesh_renderer_builder(device); }
};
inline constexpr MeshRendererBuilderFactory mesh_renderer_builder{};

template<class Mesh, class P, class U, class C>
resources::Result<MeshSource> mesh_source(const Mesh& mesh, MeshFields<P, U, C> fields)
{
    static_assert(Mesh::has(P{}), "Mesh renderer requires its Vec3 position semantic");
    static_assert(std::same_as<typename P::value_type, Vec3>);
    static_assert(std::same_as<typename U::value_type, Vec2>);
    static_assert(std::same_as<typename C::value_type, Vec4>);
    if (auto valid = mesh.validate(); !valid) return std::unexpected(resources::to_diagnostic(valid.error()));
    auto result = std::make_shared<SurfaceMesh>(mesh.vertex_count());
    result->info() = mesh.info(); result->faces() = mesh.faces();
    result->explicit_edges() = mesh.explicit_edges();
    for (std::size_t i = 0; i < mesh.vertex_count(); ++i) {
        Vec3 p{}; Vec2 uv{}; Vec4 color{1, 1, 1, 1};
        mesh.for_each_vertex_stream([&](const auto& stream) {
            using Record = typename std::remove_cvref_t<decltype(stream)>::record_type;
            if constexpr (Record::has(P{})) p = stream[i].get(fields.position);
            if constexpr (Record::has(U{})) uv = stream[i].get(fields.uv);
            if constexpr (Record::has(C{})) color = stream[i].get(fields.color);
        });
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)
            || !std::isfinite(uv.x) || !std::isfinite(uv.y)
            || !std::isfinite(color.x) || !std::isfinite(color.y)
            || !std::isfinite(color.z) || !std::isfinite(color.w))
            return std::unexpected(resources::Diagnostic{.code = resources::ErrorCode::invalid_argument,
                .message = "Mesh source contains a non-finite attribute"});
        auto& out = result->vertices()[i];
        out.set(gfx::Position{}, p); out.set(gfx::TexCoord<>{}, uv); out.set(gfx::Color{}, color);
    }
    return MeshSource{std::move(result)};
}

} // namespace vng::render
