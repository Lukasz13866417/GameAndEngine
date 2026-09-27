#pragma once
#include "../support/earth_tunnel_sizes.hpp"
#include <vng/gfx/geometry.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/render/program.hpp>
#include <vng/render/view.hpp>
#include <vng/resources/diagnostic.hpp>
#include <vng/shader/shader.hpp>

namespace editor_example {
// Depth-tested veil across the tunnel mouth, drawn AFTER opaque geometry.
// Covers stars, terminal and planet alike; near walls/ships occlude it. A far-
// depth background pass cannot veil opaque scenery drawn over the stars.
class PortalHaze {
    struct Position:vng::gfx::Semantic<vng::Vec2>{};
    using Vertex=vng::gfx::Record<Position>;
    using Program=vng::opengl::TypedProgram<vng::f32>;
    Program program_;
    vng::opengl::GpuMesh<Vertex> mesh_;
    PortalHaze(Program program,vng::opengl::GpuMesh<Vertex> mesh):program_(std::move(program)),mesh_(std::move(mesh)){}
public:
    static vng::resources::Result<PortalHaze> create(vng::opengl::Device& device) {
        using namespace vng;using resources::into_result;
        auto vertex=shader::vertex<shader::VertexInputs<Position>,shader::VertexOutputs<shader::ClipPosition>>(
            "portal_haze_vertex",[](auto& s){
                // One kilometre before the throat, ahead of its protruding rim.
                return s.output(dsl::field<shader::ClipPosition>(s.camera().project(dsl::vec3(s.input(Position{})*
                    s.constant(example::earth::cinematic_tunnel_radius_km*1.05F),1.F))));
            });
        auto fragment=shader::fragment<shader::FragmentInputs<>,shader::FragmentOutputs<shader::Color<0>>>(
            "portal_haze_fragment",[](auto& s,dsl::Float distance){
                const auto path=dsl::max(distance-1.F,0.F);
                const auto alpha=1.F-dsl::exp(-path*path/(18.F*18.F));
                return s.output(dsl::field<shader::Color<0>>(dsl::vec4(s.constant(Vec3{4.2F,4.05F,3.7F})*alpha,alpha)));
            });
        if(!vertex)return std::unexpected(resources::to_diagnostic(vertex.error()));
        if(!fragment)return std::unexpected(resources::to_diagnostic(fragment.error()));
        auto linked=into_result(shader::link(std::move(*vertex),std::move(*fragment)));if(!linked)return std::unexpected(linked.error());
        auto program=into_result(render::compile_program(device,*linked));if(!program)return std::unexpected(program.error());
        gfx::Mesh<Vertex> mesh{4};
        mesh.vertices()[0].set(Position{},Vec2{-1,-1});mesh.vertices()[1].set(Position{},Vec2{1,-1});
        mesh.vertices()[2].set(Position{},Vec2{1,1});mesh.vertices()[3].set(Position{},Vec2{-1,1});
        mesh.add_face(0,1,2);mesh.add_face(0,2,3);
        auto gpu=into_result(opengl::upload_mesh(device,mesh));if(!gpu)return std::unexpected(gpu.error());
        return PortalHaze{std::move(*program),std::move(*gpu)};
    }
    vng::resources::Result<void> render(vng::opengl::Frame& frame,const vng::render::RenderView& view,vng::f32 distance) {
        using namespace vng;using resources::into_result;
        auto commands=frame.render_context();auto graphics=commands.graphics_state();
        if(auto r=into_result(commands.run(program_,distance));!r)return r;
        if(auto r=into_result(commands.view(view));!r)return r;
        if(auto r=into_result(graphics.set(render::DepthState{true,false,render::DepthCompare::less_equal}));!r)return r;
        if(auto r=into_result(graphics.set(render::CullMode::none));!r)return r;
        if(auto r=into_result(graphics.set(render::BlendMode::premultiplied_alpha));!r)return r;
        if(auto r=into_result(graphics.set(opengl::PolygonMode::fill));!r)return r;
        return into_result(commands.draw(mesh_));
    }
};
}
