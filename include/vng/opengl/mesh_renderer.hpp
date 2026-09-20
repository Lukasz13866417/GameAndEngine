#pragma once

#include <optional>
#include <vng/opengl/device.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/opengl/image.hpp>
#include <vng/opengl/program.hpp>
#include <vng/render/mesh_renderer.hpp>
#include <vng/opengl/renderer.hpp>

namespace vng::opengl {

using MeshProvider = resources::Provider<render::MeshSource, Device>;
// Custom shaders consume render::surface::Instance semantics alongside mesh
// attributes, camera and optional texture slot 0. Explicit uniform arguments
// are not a SurfaceDraw contract and are rejected before replacement commits.
using SurfaceProgram = Program;
using ProgramProvider = resources::Provider<SurfaceProgram, Device>;
using SharedImage = resources::Shared<Image2D>;
using AlbedoProvider = resources::Provider<SharedImage, Device>;

namespace detail {
inline AlbedoProvider albedo_provider(AlbedoProvider provider) { return provider; }
template<class P>
concept AlbedoSource = requires(const P& provider, Device& device) {
    resources::provide(provider, device);
} && (std::same_as<typename decltype(resources::provide(std::declval<const P&>(), std::declval<Device&>()))::value_type, Image2D>
    || std::same_as<typename decltype(resources::provide(std::declval<const P&>(), std::declval<Device&>()))::value_type, SharedImage>);

template<AlbedoSource P> AlbedoProvider albedo_provider(P provider)
{
    return resources::provider([provider = resources::share_provider(std::move(provider))](Device& device)
        -> resources::Result<SharedImage> {
        auto value = resources::into_result(resources::provide(provider, device));
        if (!value) return std::unexpected(std::move(value.error()));
        if constexpr (std::same_as<typename decltype(value)::value_type, SharedImage>) return std::move(*value);
        else return resources::share_resource(std::move(*value));
    });
}
} // namespace detail

class MeshRendererBuilder;

class MeshRenderer final : public Renderer<render::SurfaceDraw> {
    struct Impl;
public:
    class Update {
    public:
        Update(Update&&) noexcept = default;
        Update(const Update&) = delete;
        Update& mesh(MeshProvider source);
        Update& mesh(render::MeshSource ready);
        template<class P> Update& mesh(resources::Provided<render::MeshSource, P>&& ready)
        { mesh(std::move(ready.value)); mesh_provider_ = std::move(ready.provider); return *this; }
        Update& program(ProgramProvider source);
        Update& program(SurfaceProgram&& ready);
        template<class P> Update& program(resources::Provided<SurfaceProgram, P>&& ready)
        { program(std::move(ready.value)); program_provider_ = std::move(ready.provider); return *this; }
        Update& albedo(SharedImage ready);
        Update& albedo(Image2D&& ready) { return albedo(resources::share_resource(std::move(ready))); }
        template<detail::AlbedoSource P> Update& albedo(P source)
        { albedo_changed_ = true; albedo_ready_.reset(); albedo_provider_ = detail::albedo_provider(std::move(source)); return *this; }
        template<class P> Update& albedo(resources::Provided<Image2D, P>&& ready)
        { albedo(std::move(ready.value)); albedo_provider_ = detail::albedo_provider(std::move(ready.provider)); return *this; }
        template<class P> Update& albedo(resources::Provided<SharedImage, P>&& ready)
        { albedo(std::move(ready.value)); albedo_provider_ = detail::albedo_provider(std::move(ready.provider)); return *this; }
        // Inputs are captured before invoking any provider. Setters called
        // from a provider cannot change this attempt or make it reusable.
        [[nodiscard]] resources::Result<void> commit();
    private:
        Update(std::weak_ptr<Impl> owner, Device& device, u64 generation);
        std::weak_ptr<Impl> owner_;
        Device* device_{};
        u64 generation_{};
        bool committed_{}, mesh_changed_{}, program_changed_{}, albedo_changed_{};
        MeshProvider mesh_provider_;
        ProgramProvider program_provider_;
        AlbedoProvider albedo_provider_;
        std::optional<render::MeshSource> mesh_ready_;
        std::optional<SurfaceProgram> program_ready_;
        std::optional<SharedImage> albedo_ready_;
        friend class MeshRenderer;
    };

    MeshRenderer(MeshRenderer&&) noexcept = default;
    MeshRenderer& operator=(MeshRenderer&&) noexcept = default;
    MeshRenderer(const MeshRenderer&) = delete;
    MeshRenderer& operator=(const MeshRenderer&) = delete;
    ~MeshRenderer();

    [[nodiscard]] Update update(Device& device);
    [[nodiscard]] resources::Result<resources::ReloadReport> reload(Device& device);
    [[nodiscard]] resources::Result<void> reload_mesh(Device& device);
    [[nodiscard]] resources::Result<void> reload_mesh(Device& device, MeshProvider source);
    [[nodiscard]] resources::Result<void> reload_program(Device& device);
    [[nodiscard]] resources::Result<void> reload_program(Device& device, ProgramProvider source);
    [[nodiscard]] resources::Result<void> reload_albedo(Device& device);
    template<detail::AlbedoSource P> resources::Result<void> reload_albedo(Device& device, P source)
    { auto pending = update(device); pending.albedo(std::move(source)); return pending.commit(); }
    [[nodiscard]] resources::Result<void> replace_mesh(Device& device, render::MeshSource ready);
    template<class P> resources::Result<void> replace_mesh(Device& device, resources::Provided<render::MeshSource,P>&& ready)
    { auto pending = update(device); pending.mesh(std::move(ready)); return pending.commit(); }
    [[nodiscard]] resources::Result<void> replace_program(Device& device, SurfaceProgram&& ready);
    template<class P> resources::Result<void> replace_program(Device& device, resources::Provided<SurfaceProgram,P>&& ready)
    { auto pending = update(device); pending.program(std::move(ready)); return pending.commit(); }
    [[nodiscard]] resources::Result<void> replace_albedo(Device& device, SharedImage ready);
    [[nodiscard]] resources::Result<void> replace_albedo(Device& device, Image2D&& ready)
    { return replace_albedo(device, resources::share_resource(std::move(ready))); }
    template<class P> resources::Result<void> replace_albedo(Device& device, resources::Provided<Image2D,P>&& ready)
    { auto pending = update(device); pending.albedo(std::move(ready)); return pending.commit(); }
    template<class P> resources::Result<void> replace_albedo(Device& device, resources::Provided<SharedImage,P>&& ready)
    { auto pending = update(device); pending.albedo(std::move(ready)); return pending.commit(); }

    [[nodiscard]] resources::Result<void> render(Frame&, const render::RenderView&,
        std::span<const render::SurfaceDraw>);
    [[nodiscard]] resources::Result<void> render(Frame& frame, const render::RenderView& view,
        const render::SurfaceDraw& draw)
    { return render(frame, view, std::span{&draw,1}); }
    [[nodiscard]] const render::SurfaceMesh& source_mesh() const;
    [[nodiscard]] const SurfaceProgram& program() const;
    [[nodiscard]] const Image2D& albedo() const;
    [[nodiscard]] u64 revision() const noexcept;
    [[nodiscard]] render::SurfaceRenderStats stats() const noexcept;
private:
    explicit MeshRenderer(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<Impl> impl_;
    friend class MeshRendererBuilder;
};

class MeshRendererBuilder {
public:
    explicit MeshRendererBuilder(Device& device);
    MeshRendererBuilder& mesh(MeshProvider source) { mesh_ = std::move(source); return *this; }
    MeshRendererBuilder& program(ProgramProvider source) { program_ = std::move(source); return *this; }
    template<detail::AlbedoSource P> MeshRendererBuilder& albedo(P source)
    { albedo_ = detail::albedo_provider(std::move(source)); return *this; }
    [[nodiscard]] resources::Result<MeshRenderer> build() const;
private:
    Device* device_;
    MeshProvider mesh_;
    ProgramProvider program_;
    AlbedoProvider albedo_;
};

inline MeshRendererBuilder make_backend_mesh_renderer_builder(Device& device)
{ return MeshRendererBuilder{device}; }

} // namespace vng::opengl
