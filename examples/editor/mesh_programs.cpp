#include "mesh_programs.hpp"
namespace editor_example {
vng::resources::Result<MeshPrograms::Entry*> MeshPrograms::provide(vng::opengl::Device& device,
                                                                 mesh_shading::LightingStyle style) {
    if(auto found=entries_.find(style);found!=entries_.end())return found->second.get();
    auto instanced=BlueprintMeshRenderer::create_program(device,style);
    if(!instanced)return std::unexpected(instanced.error());
    auto neutral=mesh_shading::shader_program(style);
    if(!neutral)return std::unexpected(vng::resources::to_diagnostic(neutral.error()));
    auto diagnostic=mesh_shading::Program::create(device,std::move(*neutral));
    if(!diagnostic)return std::unexpected(vng::resources::to_diagnostic(diagnostic.error()));
    auto entry=std::make_unique<Entry>(std::move(*instanced),std::move(*diagnostic));
    auto* result=entry.get();entries_.emplace(style,std::move(entry));return result;
}
}
