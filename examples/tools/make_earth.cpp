// Offline authoring only. Never overwrite an edited asset implicitly.
#include "../scenes/earth_scene.hpp"
#include "../support/earth_assets.hpp"
#include "../editor/scene_file.hpp"
#include "../editor/animation.hpp"
#include <iostream>

int main(int argc,char** argv) {
    const bool savannah=argc==4&&std::string_view(argv[3])=="--savannah";
    const bool redesign=argc>=4&&std::string_view(argv[3])=="--redesign-infrastructure";
    const bool global=argc>=4&&std::string_view(argv[3])=="--global-infrastructure";
    const bool retune=global||redesign||(argc>=4&&std::string_view(argv[3])=="--tunnel-classes");
    const bool replace=retune&&argc==5&&std::string_view(argv[4])=="--replace";
    const bool refresh=retune||(argc==4&&std::string_view(argv[3])=="--refresh-future");
    const bool future=refresh||(argc==4&&std::string_view(argv[3])=="--future");
    if((argc!=3&&!savannah&&!future)||argc>5||(argc==5&&!replace)) {std::cerr<<"Usage: vng_make_earth SOURCE_ASSET_DIRECTORY OUTPUT_DIRECTORY [--savannah|--future|--refresh-future|--tunnel-classes [--replace]|--redesign-infrastructure [--replace]|--global-infrastructure [--replace]]\n";return 2;}
    const std::filesystem::path output=argv[2];
    const auto stem=future?"earth_future":savannah?"earth_savannah":"earth";
    const auto mesh_path=output/(std::string(stem)+".vmesh"),scene_path=output/(std::string(stem)+".vscene");
    if(!std::filesystem::is_directory(output) || (!replace&&(std::filesystem::exists(mesh_path) || std::filesystem::exists(scene_path)))) {
        std::cerr<<"Output directory must exist and both output files must be new.\n";return 1;
    }
    if(savannah||future) {
        namespace project=editor_example;
        const std::filesystem::path input=argv[1];
        auto mesh=vng::content::vmesh::read_vmesh(input/(refresh?"earth_future.vmesh":"earth.vmesh"));
        if(!mesh){std::cerr<<mesh.error().message<<'\n';return 1;}
        const auto change=[future,refresh,retune,redesign,global](const vng::content::vmesh::Document& source) {
            if(!future)return example::earth::make_savannah_variant(source);
            if(global)return example::earth::expand_global_infrastructure(source);
            if(redesign)return example::earth::redesign_infrastructure(source);
            if(retune)return example::earth::author_tunnel_network(source);
            auto settings=refresh?example::earth::infrastructure_settings(source)
                :vng::content::Result<example::earth::InfrastructureSettings>{{true,true,true}};
            if(!settings)return vng::content::Result<vng::content::vmesh::Document>{std::unexpected(settings.error())};
            auto result=example::earth::rebuild_infrastructure(source,*settings);
            if(result&&!refresh) {
                result=example::earth::redesign_infrastructure(*result);
                if(!result)return result;
                result=example::earth::expand_global_infrastructure(*result);
                if(!result)return result;
                result->metadata["name"]="EARTH / connected homeworld";
                result->metadata["editor/earth-variant"]="future";
            }
            return result;
        };
        auto variant=change(*mesh);
        if(!variant){std::cerr<<variant.error().message<<'\n';return 1;}
        project::SceneFile file;auto scene=file.load(input/(refresh?"earth_future.vscene":"earth.vscene"));
        if(!scene){std::cerr<<scene.error().message<<'\n';return 1;}
        const auto rebuild=[&change](vng::editor::EditableMesh& geometry) -> vng::content::Result<void> {
            auto changed=change(geometry.document());
            if(!changed)return std::unexpected(changed.error());
            auto edited=vng::editor::EditableMesh::create(std::move(*changed));
            if(!edited)return std::unexpected(edited.error());
            geometry=std::move(*edited);return {};
        };
        for(auto& asset:scene->document.mesh_assets)if(example::earth::is_earth(asset.geometry.document())) {
            if(auto changed=rebuild(asset.geometry);!changed){std::cerr<<changed.error().message<<'\n';return 1;}
            if(!refresh) {
                asset.name=future?"EARTH / connected homeworld":"EARTH / savannah homeworld";
                for(auto& instance:scene->document.instances)if(instance.blueprint==asset.id)instance.name=future?"Earth / connected":"Earth / savannah";
            }
        }
        for(auto& [id,draft]:scene->document.mesh_drafts)if(example::earth::is_earth(draft.document()))
            if(auto changed=rebuild(draft);!changed){std::cerr<<changed.error().message<<'\n';return 1;}
        if(future&&!refresh) {
            auto camera=project::ensure_camera(*scene,{100,10,3.4F,{},1},"Homeworld orbit camera");
            if(!camera){std::cerr<<camera.error().message<<'\n';return 1;}
            std::vector<vng::u32> cameras;
            for(auto& instance:scene->document.instances)if(std::holds_alternative<project::CameraSettings>(instance.settings)) {
                project::place_camera(instance,{100,10,3.4F,{},1});cameras.push_back(instance.id);
            }
            // Use normal editable camera instances and keys, including when
            // the source file still used the legacy animation-camera property.
            for(auto id:cameras)for(auto time:{0.F,45.F,90.F}) {
                auto keyed=project::key_camera(*scene,id,time,{100+time*1.2F,10,3.4F,{},1},vng::timeline::Interpolation::linear);
                if(!keyed){std::cerr<<keyed.error().message<<'\n';return 1;}
            }
            scene->document.keyframe_names={{0,"Dusk / connected homeworld"},{45,"Night / regional skyways"},{90,"Night / launch hubs"}};
            scene->viewport.editor_camera=project::evaluate_camera(*scene,0);
            scene->document.environment.bloom_threshold=1.6F;
            scene->document.environment.bloom_strength=.16F;
        }
        if(auto saved=vng::content::vmesh::write_vmesh(mesh_path,*variant);!saved){std::cerr<<saved.error().message<<'\n';return 1;}
        if(auto saved=file.save_as(scene_path,*scene,replace);!saved){std::cerr<<saved.error().message<<'\n';return 1;}
        std::cout<<"Copied Earth to "<<mesh_path<<" and "<<scene_path<<" (original terrain/clouds retained)\n";
        return 0;
    }
    auto scene=example::earth::author_scene(argv[1]);
    if(!scene){std::cerr<<scene.error().message<<'\n';return 1;}
    const auto& mesh=editor_example::mesh_geometry(*scene,example::earth::blueprint_id)->document();
    if(auto saved=vng::content::vmesh::write_vmesh(mesh_path,mesh);!saved){std::cerr<<saved.error().message<<'\n';return 1;}
    editor_example::SceneFile file;
    if(auto saved=file.save_as(scene_path,*scene);!saved){std::cerr<<saved.error().message<<'\n';return 1;}
    std::cout<<"Earth: "<<mesh.vertex_count<<" vertices / "<<mesh.faces.size()<<" triangles\n";
}
