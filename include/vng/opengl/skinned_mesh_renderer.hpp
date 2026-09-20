#pragma once

#include <cmath>
#include <expected>
#include <memory>
#include <span>

#include <vng/analysis/evidence.hpp>
#include <vng/analysis/request.hpp>
#include <vng/gfx/mesh.hpp>
#include <vng/glsl/emitter.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/renderer.hpp>
#include <vng/render/skinned_mesh_renderer.hpp>
#include <vng/resources/resources.hpp>

namespace vng::opengl {
class Device;
class Frame;
class SkinnedMeshRendererBuilder;

namespace detail {
using SkinnedGeometry = gfx::Record<gfx::Position, gfx::Normal, gfx::Color>;
inline Diagnostic skin_diagnostic(const rig::Diagnostic& cause)
{
    auto message = "Skinned mesh: " + cause.message;
    if (cause.vertex) message += " (vertex " + std::to_string(*cause.vertex) + ")";
    return {.code = ErrorCode::invalid_argument, .message = std::move(message)};
}
struct SkinnedMeshSource final {
    gfx::Mesh<SkinnedGeometry> mesh;
    rig::SkinWeights weights;
    bool has_normals{};
};

template<class Mesh, class P, class N, class C>
[[nodiscard]] std::expected<SkinnedMeshSource, Diagnostic> decode_skin_source(
    const rig::SkinBinding<Mesh>& binding, render::GeometryFields<P, N, C> fields);

template<class P, class Fields>
[[nodiscard]] auto adapt_skin_provider(P&& source, Fields fields);
} // namespace detail

class SkinnedMeshRenderer final : public Renderer<render::SkinnedDraw> {
public:
    using SkinProvider = resources::Provider<detail::SkinnedMeshSource, Device>;
    SkinnedMeshRenderer(SkinnedMeshRenderer&&) noexcept;
    SkinnedMeshRenderer& operator=(SkinnedMeshRenderer&&) noexcept;
    SkinnedMeshRenderer(const SkinnedMeshRenderer&) = delete;
    SkinnedMeshRenderer& operator=(const SkinnedMeshRenderer&) = delete;
    ~SkinnedMeshRenderer();

    [[nodiscard]] std::expected<void, Diagnostic> render(
        Frame& frame, const render::RenderView& view,
        std::span<const render::SkinnedDraw> draws);
    [[nodiscard]] std::expected<void, Diagnostic> render(
        Frame& frame, const render::RenderView& view, const render::SkinnedDraw& draw);
    [[nodiscard]] std::expected<analysis::FrameEvidence, Diagnostic> capture(
        Frame& frame, const render::RenderView& view, const render::SkinnedDraw& draw,
        const analysis::CaptureRequest& request = analysis::CaptureRequest::standard());

    [[nodiscard]] render::SkinnedRenderStats stats() const noexcept;
    [[nodiscard]] const glsl::ProgramSource& source() const;

    [[nodiscard]] resources::Result<void> reload_skin(Device& device);
    [[nodiscard]] resources::Result<void> reload_skin(Device& device, SkinProvider provider);
    template<class P, class Position = gfx::Position, class Normal = gfx::Normal, class Color = gfx::Color>
        requires (!std::same_as<std::remove_cvref_t<P>, SkinProvider>)
            && requires(const P& source, Device& device) { resources::provide(source, device); }
    [[nodiscard]] resources::Result<void> reload_skin(Device& device, P&& provider,
        render::GeometryFields<Position, Normal, Color> fields = {})
    {
        return reload_skin(device, SkinProvider{
            detail::adapt_skin_provider(std::forward<P>(provider), fields)});
    }

    template<class Mesh, class Position = gfx::Position, class Normal = gfx::Normal, class Color = gfx::Color>
    [[nodiscard]] resources::Result<void> replace_skin(Device& device,
        const rig::SkinBinding<Mesh>& binding,
        render::GeometryFields<Position, Normal, Color> fields = {})
    {
        if (auto valid = validate_update(device); !valid) return valid;
        if (auto valid = binding.validate(); !valid)
            return std::unexpected(resources::to_diagnostic(std::move(valid.error())));
        auto source = detail::decode_skin_source(binding, fields);
        if (!source) return std::unexpected(resources::to_diagnostic(std::move(source.error())));
        return install_skin(device, std::move(*source), {});
    }

    template<class Mesh, class P, class Position = gfx::Position, class Normal = gfx::Normal, class Color = gfx::Color>
    [[nodiscard]] resources::Result<void> replace_skin(Device& device,
        resources::Provided<rig::SkinBinding<Mesh>, P> binding,
        render::GeometryFields<Position, Normal, Color> fields = {})
    {
        if (auto valid = validate_update(device); !valid) return valid;
        if (auto valid = binding.value.validate(); !valid)
            return std::unexpected(resources::to_diagnostic(std::move(valid.error())));
        auto source = detail::decode_skin_source(binding.value, fields);
        if (!source) return std::unexpected(resources::to_diagnostic(std::move(source.error())));
        return install_skin(device, std::move(*source), SkinProvider{
            detail::adapt_skin_provider(std::move(binding.provider), fields)});
    }
    [[nodiscard]] resources::Result<resources::ReloadReport> reload(Device& device);

private:
    friend class SkinnedMeshRendererBuilder;
    [[nodiscard]] resources::Result<void> validate_update(const Device& device) const;
    [[nodiscard]] resources::Result<void> install_skin(
        Device& device, detail::SkinnedMeshSource source, SkinProvider provider);
    [[nodiscard]] static std::expected<SkinnedMeshRenderer, Diagnostic> create(
        Device& device, detail::SkinnedMeshSource source,
        render::SkinnedRendererOptions options);
    template<class Mesh, class P, class N, class C>
    friend std::expected<SkinnedMeshRenderer, Diagnostic> make_backend_skinned_mesh_renderer(
        Device&, const rig::SkinBinding<Mesh>&,
        render::GeometryFields<P, N, C>, render::SkinnedRendererOptions);
    struct Impl;
    explicit SkinnedMeshRenderer(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
    bool providing_{};
};

class SkinnedMeshRendererBuilder final {
public:
    explicit SkinnedMeshRendererBuilder(Device& device) : device_(&device) {}

    template<class P, class Position = gfx::Position, class Normal = gfx::Normal, class Color = gfx::Color>
    SkinnedMeshRendererBuilder& skin(P&& source,
        render::GeometryFields<Position, Normal, Color> fields = {})
    {
        provider_ = SkinnedMeshRenderer::SkinProvider{
            detail::adapt_skin_provider(std::forward<P>(source), fields)};
        return *this;
    }
    SkinnedMeshRendererBuilder& options(render::SkinnedRendererOptions options)
    {
        options_ = options;
        return *this;
    }
    [[nodiscard]] resources::Result<SkinnedMeshRenderer> build() const;

private:
    Device* device_;
    SkinnedMeshRenderer::SkinProvider provider_;
    render::SkinnedRendererOptions options_;
};

[[nodiscard]] inline SkinnedMeshRendererBuilder make_backend_skinned_mesh_renderer_builder(Device& device)
{
    return SkinnedMeshRendererBuilder{device};
}

// The asset boundary decodes logical fields once. The supplied record's
// encoding, field order, and number of streams remain the application's choice.
template<class Mesh, class P, class N, class C>
[[nodiscard]] std::expected<detail::SkinnedMeshSource, Diagnostic>
detail::decode_skin_source(
    const rig::SkinBinding<Mesh>& binding, render::GeometryFields<P, N, C> fields)
{
    static_assert(Mesh::has(P{}), "Skinned mesh must contain the position semantic");
    static_assert(std::same_as<typename P::value_type, Vec3>, "Position must be Vec3");
    static_assert(std::same_as<typename N::value_type, Vec3>, "Normal must be Vec3");
    static_assert(std::same_as<typename C::value_type, Vec4>, "Color must be Vec4");
    if (auto valid = binding.validate(); !valid) {
        return std::unexpected(detail::skin_diagnostic(valid.error()));
    }
    const auto& mesh = binding.mesh();
    gfx::Mesh<detail::SkinnedGeometry> canonical(mesh.vertex_count());
    canonical.info() = mesh.info();
    canonical.faces() = mesh.faces();
    for (std::size_t i = 0; i < mesh.vertex_count(); ++i) {
        Vec3 position{}, normal{0, 0, 1};
        Vec4 color{1, 1, 1, 1};
        mesh.for_each_vertex_stream([&](const auto& stream) {
            using Record = typename std::remove_cvref_t<decltype(stream)>::record_type;
            if constexpr (Record::has(P{})) position = stream[i].get(fields.position);
            if constexpr (Record::has(N{})) normal = stream[i].get(fields.normal);
            if constexpr (Record::has(C{})) color = stream[i].get(fields.color);
        });
        for (u32 component = 0; component < 3; ++component) {
            if (!std::isfinite(position[component]) || !std::isfinite(normal[component])) {
                return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
                    .message = "Skinned mesh positions and normals must be finite"});
            }
        }
        const double length2 = static_cast<double>(normal.x) * normal.x
            + static_cast<double>(normal.y) * normal.y + static_cast<double>(normal.z) * normal.z;
        if (!(length2 > 0)) {
            return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
                .message = "Skinned mesh normals must have nonzero length"});
        }
        // Logical normals have direction, not magnitude. Normalize in double
        // before upload so a finite authored length cannot overflow shader dot.
        const auto length = std::sqrt(length2);
        normal = {static_cast<f32>(normal.x / length),
                  static_cast<f32>(normal.y / length),
                  static_cast<f32>(normal.z / length)};
        for (u32 component = 0; component < 4; ++component) {
            if (!std::isfinite(color[component])) {
                return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
                    .message = "Skinned mesh colors must be finite"});
            }
        }
        auto& vertex = canonical.vertices()[i];
        vertex.set(gfx::Position{}, position);
        vertex.set(gfx::Normal{}, normal);
        vertex.set(gfx::Color{}, color);
    }
    return SkinnedMeshSource{std::move(canonical), binding.weights(), Mesh::has(N{})};
}

template<class P, class Fields>
[[nodiscard]] auto detail::adapt_skin_provider(P&& source, Fields fields)
{
    return resources::provider(
        [source = std::forward<P>(source), fields](Device& device)
            -> resources::Result<SkinnedMeshSource> {
            auto binding = resources::provide(source, device);
            if (!binding) return std::unexpected(resources::to_diagnostic(std::move(binding.error())));
            if constexpr (std::same_as<typename decltype(binding)::value_type, SkinnedMeshSource>) {
                return std::move(*binding);
            } else {
                if (auto valid = binding->validate(); !valid)
                    return std::unexpected(resources::to_diagnostic(std::move(valid.error())));
                return resources::into_result(decode_skin_source(*binding, fields));
            }
        });
}

template<class Mesh, class P, class N, class C>
[[nodiscard]] std::expected<SkinnedMeshRenderer, Diagnostic>
make_backend_skinned_mesh_renderer(
    Device& device, const rig::SkinBinding<Mesh>& binding,
    render::GeometryFields<P, N, C> fields, render::SkinnedRendererOptions options)
{
    auto source = detail::decode_skin_source(binding, fields);
    if (!source) return std::unexpected(std::move(source.error()));
    return SkinnedMeshRenderer::create(device, std::move(*source), options);
}

template<class Mesh, class P, class N, class C>
[[nodiscard]] std::expected<SkinnedMeshRenderer, Diagnostic>
make_backend_skinned_mesh_renderer(
    Device& device, const rig::SkinBinding<Mesh>& binding,
    render::GeometryFields<P, N, C> fields)
{
    return make_backend_skinned_mesh_renderer(device, binding, fields, {});
}

template<class Mesh>
[[nodiscard]] std::expected<SkinnedMeshRenderer, Diagnostic>
make_backend_skinned_mesh_renderer(
    Device& device, const rig::SkinBinding<Mesh>& binding,
    render::SkinnedRendererOptions options = {})
{
    return make_backend_skinned_mesh_renderer(device, binding, render::GeometryFields{}, options);
}

} // namespace vng::opengl
