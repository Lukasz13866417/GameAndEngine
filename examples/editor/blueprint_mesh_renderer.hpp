#pragma once
#include "mesh_shading.hpp"
#include <vng/opengl/frame.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/opengl/renderer.hpp>

namespace editor_example {
// Runtime owns the shared program before its blueprint renderers. This
// explicit dependency stays valid across renderer moves and mesh replacement.
class BlueprintMeshRenderer final : public vng::opengl::Renderer<mesh_shading::MeshDraw> {
public:
    using GpuMesh=vng::opengl::GpuMesh<mesh_shading::Vertex,mesh_shading::Surface>;
    struct Stats {vng::u32 draw_calls{}, instances{};};
    static vng::resources::Result<vng::opengl::Program> create_program(vng::opengl::Device&,
        mesh_shading::LightingStyle = mesh_shading::LightingStyle::standard);
    BlueprintMeshRenderer(GpuMesh mesh,const vng::opengl::Program& program)
        :mesh_(std::move(mesh)),program_(&program) {}
    vng::resources::Result<void> render(vng::opengl::Frame&,const vng::render::RenderView&,
        std::span<const mesh_shading::MeshDraw>);
    GpuMesh& mesh() {return mesh_;}
    Stats stats() const {return stats_;}
private:
    template<unsigned Column> struct ModelColumn : vng::gfx::Semantic<vng::Vec4> {};
    using Instance=vng::gfx::Record<ModelColumn<0>,ModelColumn<1>,ModelColumn<2>,ModelColumn<3>,
        mesh_shading::Eye,mesh_shading::Light,mesh_shading::Brightness>;
    // These children partition only incompatible raster state. Ordinary value
    // differences (including lighting) never split a batch.
    struct Batch {
        vng::opengl::InstanceBuffer<Instance> gpu;
        std::vector<Instance> records;
    };
    GpuMesh mesh_;
    const vng::opengl::Program* program_;
    std::array<Batch,2> batches_;
    Stats stats_;
};
} // namespace editor_example
