#include "blueprint_mesh_renderer.hpp"
#include <vng/render/program.hpp>

namespace editor_example {
using namespace vng;
using namespace mesh_shading;

resources::Result<opengl::Program> BlueprintMeshRenderer::create_program(opengl::Device& device,LightingStyle style) {
    using VI=shader::VertexInputs<Position,Color,Normal,Emission,ModelColumn<0>,ModelColumn<1>,
        ModelColumn<2>,ModelColumn<3>,Eye,Light,Brightness>;
    using VO=shader::VertexOutputs<shader::ClipPosition,shader::smooth<Color>,shader::smooth<WorldPosition>,
        shader::smooth<WorldNormal>,shader::smooth<Emission>,shader::flat<Eye>,shader::flat<Light>,shader::flat<Brightness>>;
    using FI=shader::FragmentInputs<shader::smooth<Color>,shader::smooth<WorldPosition>,
        shader::smooth<WorldNormal>,shader::smooth<Emission>,shader::flat<Eye>,shader::flat<Light>,shader::flat<Brightness>>;
    auto vertex=shader::vertex<VI,VO>("editor_mesh_instances",[](auto& s) {
        const auto model=dsl::make<Mat4>(s.input(ModelColumn<0>{}),s.input(ModelColumn<1>{}),
            s.input(ModelColumn<2>{}),s.input(ModelColumn<3>{}));
        const auto world=model*dsl::vec4(s.input(Position{}),1.0F);
        const auto normal=transform_normal(model,s.input(Normal{}));
        return s.output(dsl::field<shader::ClipPosition>(s.camera().project(world.xyz())),
            dsl::field<Color>(s.input(Color{})),dsl::field<WorldPosition>(world.xyz()),
            dsl::field<WorldNormal>(normal),dsl::field<Emission>(s.input(Emission{})),
            dsl::field<Eye>(s.input(Eye{})),dsl::field<Light>(s.input(Light{})),
            dsl::field<Brightness>(s.input(Brightness{})));
    });
    auto fragment=shader::fragment<FI,shader::FragmentOutputs<shader::Color<0>>>("editor_mesh_instances",[style](auto& s) {
        return shade(s,dsl::make<Lighting>(dsl::field<Eye>(s.input(Eye{})),
            dsl::field<Light>(s.input(Light{})),dsl::field<Brightness>(s.input(Brightness{}))),style);
    });
    if(!vertex)return std::unexpected(resources::to_diagnostic(vertex.error()));
    if(!fragment)return std::unexpected(resources::to_diagnostic(fragment.error()));
    auto linked=shader::link(std::move(*vertex),std::move(*fragment));
    if(!linked)return std::unexpected(resources::to_diagnostic(linked.error()));
    return resources::into_result(render::compile_program(device,*linked));
}

resources::Result<void> BlueprintMeshRenderer::render(opengl::Frame& frame,const render::RenderView& view,
    std::span<const MeshDraw> tickets) {
    stats_={};
    for(auto& batch:batches_)batch.records.clear();
    for(const auto& ticket:tickets) {
        Instance record;
        const auto column=[&](unsigned c){return Vec4{ticket.transform[c][0],ticket.transform[c][1],ticket.transform[c][2],ticket.transform[c][3]};};
        record.set(ModelColumn<0>{},column(0));record.set(ModelColumn<1>{},column(1));
        record.set(ModelColumn<2>{},column(2));record.set(ModelColumn<3>{},column(3));
        record.set(Eye{},ticket.lighting.get(Eye{}));record.set(Light{},ticket.lighting.get(Light{}));
        record.set(Brightness{},ticket.lighting.get(Brightness{}));
        batches_[ticket.wireframe?1:0].records.push_back(record);
    }
    if(tickets.empty())return {};
    auto commands=frame.render_context();
    auto graphics=commands.graphics_state();
    if(auto r=commands.run(*program_);!r)return std::unexpected(resources::to_diagnostic(r.error()));
    if(auto r=commands.view(view);!r)return std::unexpected(resources::to_diagnostic(r.error()));
    if(auto r=graphics.set(render::DepthState{true,true});!r)return std::unexpected(resources::to_diagnostic(r.error()));
    if(auto r=graphics.set(render::CullMode::none);!r)return std::unexpected(resources::to_diagnostic(r.error()));
    if(auto r=graphics.set(render::BlendMode::disabled);!r)return std::unexpected(resources::to_diagnostic(r.error()));
    for(std::size_t i=0;i<batches_.size();++i) {
        auto& batch=batches_[i];if(batch.records.empty())continue;
        if(auto r=batch.gpu.update(frame.device(),batch.records);!r)return std::unexpected(resources::to_diagnostic(r.error()));
        if(auto r=graphics.set(i?opengl::PolygonMode::line:opengl::PolygonMode::fill);!r)return std::unexpected(resources::to_diagnostic(r.error()));
        if(auto r=commands.draw(mesh_,batch.gpu);!r)return std::unexpected(resources::to_diagnostic(r.error()));
        ++stats_.draw_calls;stats_.instances+=batch.gpu.size();
    }
    return {};
}
} // namespace editor_example
