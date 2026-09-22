// Offline authoring only. Never overwrite an edited asset implicitly.
#include "../scenes/earth_scene.hpp"
#include "../support/earth_assets.hpp"
#include "../editor/scene_file.hpp"
#include <iostream>

int main(int argc,char** argv) {
    const bool savannah=argc==4&&std::string_view(argv[3])=="--savannah";
    if(argc!=3&&!savannah) {std::cerr<<"Usage: vng_make_earth SOURCE_ASSET_DIRECTORY NEW_OUTPUT_DIRECTORY [--savannah]\n";return 2;}
    const std::filesystem::path output=argv[2];
    const auto stem=savannah?"earth_savannah":"earth";
    const auto mesh_path=output/(std::string(stem)+".vmesh"),scene_path=output/(std::string(stem)+".vscene");
    if(!std::filesystem::is_directory(output) || std::filesystem::exists(mesh_path) || std::filesystem::exists(scene_path)) {
        std::cerr<<"Output directory must exist and both output files must be new.\n";return 1;
    }
    if(savannah) {
        namespace project=editor_example;
        const std::filesystem::path input=argv[1];
        auto mesh=vng::content::vmesh::read_vmesh(input/"earth.vmesh");
        if(!mesh){std::cerr<<mesh.error().message<<'\n';return 1;}
        auto variant=example::earth::make_savannah_variant(*mesh);
        if(!variant){std::cerr<<variant.error().message<<'\n';return 1;}
        project::SceneFile file;auto scene=file.load(input/"earth.vscene");
        if(!scene){std::cerr<<scene.error().message<<'\n';return 1;}
        const auto recolor=[](vng::editor::EditableMesh& geometry) -> vng::content::Result<void> {
            auto changed=example::earth::make_savannah_variant(geometry.document());
            if(!changed)return std::unexpected(changed.error());
            auto edited=vng::editor::EditableMesh::create(std::move(*changed));
            if(!edited)return std::unexpected(edited.error());
            geometry=std::move(*edited);return {};
        };
        for(auto& asset:scene->document.mesh_assets)if(example::earth::is_earth(asset.geometry.document())) {
            if(auto changed=recolor(asset.geometry);!changed){std::cerr<<changed.error().message<<'\n';return 1;}
            asset.name="EARTH / savannah homeworld";
            for(auto& instance:scene->document.instances)if(instance.blueprint==asset.id)instance.name="Earth / savannah";
        }
        for(auto& [id,draft]:scene->document.mesh_drafts)if(example::earth::is_earth(draft.document()))
            if(auto changed=recolor(draft);!changed){std::cerr<<changed.error().message<<'\n';return 1;}
        if(auto saved=vng::content::vmesh::write_vmesh(mesh_path,*variant);!saved){std::cerr<<saved.error().message<<'\n';return 1;}
        if(auto saved=file.save_as(scene_path,*scene);!saved){std::cerr<<saved.error().message<<'\n';return 1;}
        std::cout<<"Copied Earth to "<<mesh_path<<" and "<<scene_path<<" (geometry/clouds unchanged)\n";
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
