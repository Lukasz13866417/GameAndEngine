#include "earth_scene.hpp"
#include "earth_assets.hpp"
#include "../editor/animation.hpp"

namespace example::earth {
vng::content::Result<editor_example::State> author_scene(const std::filesystem::path& assets) {
    using namespace vng;
    namespace project=editor_example;
    auto cube=editor::EditableMesh::load(assets/"colored_cube.vmesh");
    if(!cube)return std::unexpected(cube.error());
    auto earth=editor::EditableMesh::create(make_mesh());
    if(!earth)return std::unexpected(earth.error());
    project::State state{.document={.mesh=std::move(*cube)}};
    state.document.mesh_assets.push_back({blueprint_id,"EARTH / stylized homeworld",std::move(*earth),{}});
    state.document.next_blueprint_id=4;
    state.document.instances={{instance_id,blueprint_id,"Earth",project::MeshSettings{},{{},{0,75,-18},1}}};
    state.document.next_instance_id=2;
    state.document.world_bounds={{-30,-30,-30},{30,30,30}};
    state.document.environment={.stars=1800,.star_seed=1386,.exposure=.9F,.bloom_threshold=3,.bloom_strength=.08F};
    state.document.animation_camera={45,8,3.7F,{},1};
    state.document.timeline_duration=90;
    state.document.keyframe_names={{0,"Atlantic / homeworld"},{45,"The Americas"},{90,"Pacific / hold"}};
    for(const auto time:{0.F,45.F,90.F}) {
        auto key=project::key_property(state,{instance_id,"rotation"},time,Vec3{0,75+time, -18});
        if(!key)return std::unexpected(key.error());
    }
    state.viewport.mode=project::ViewMode::scene;
    state.viewport.selected_object=instance_id;
    state.viewport.inspected_mesh=blueprint_id;
    state.viewport.editor_camera=state.document.animation_camera;
    return state;
}
}
