#include "earth_scene.hpp"
#include "../support/earth_assets.hpp"
#include "../support/earth_express_route.hpp"
#include "../editor/animation.hpp"

namespace example::earth {
vng::content::Result<vng::content::vmesh::Document> author_tunnel_network(const vng::content::vmesh::Document& source) {
    constexpr auto revision="earth/infrastructure/network-revision";
    if(source.metadata.contains(revision)&&source.metadata.at(revision)=="bezier-lanes-2")return source;
    const bool classified=source.metadata.contains(revision)&&source.metadata.at(revision)=="size-classes-1";
    auto settings=infrastructure_settings(source);if(!settings)return std::unexpected(settings.error());
    auto result=has_infrastructure_parts(source)?vng::content::Result<vng::content::vmesh::Document>{source}:
        rebuild_infrastructure(source,*settings);
    if(!result)return result;
    auto parts=infrastructure_parts(*result);if(!parts)return std::unexpected(parts.error());
    unsigned route{};
    for(auto p:*parts)if(!classified&&p.kind==InfrastructureKind::skyway) {
        p.tunnel_class=std::array{TunnelSizeClass::local,TunnelSizeClass::regional,
            TunnelSizeClass::trunk,TunnelSizeClass::regional}[route++%4];
        if(p.name.starts_with("Skyway "))p.name+=p.tunnel_class==TunnelSizeClass::local?" / local":
            p.tunnel_class==TunnelSizeClass::regional?" / regional":" / trunk";
        result=edit_infrastructure(*result,p);if(!result)return result;
        // Trunk is also the legacy default: an unchanged class still needs its
        // old baked width regenerated once. Never rebuild unrelated structures.
        result=rebuild_infrastructure_part(*result,p.id);if(!result)return result;
    }
    struct Feeder { const char* name; vng::Vec2 start,end; };
    if(!classified)for(const auto& feeder:std::array{
        Feeder{"Pacific local / coastal feeder",{116,34},{120,31}},
        Feeder{"India local / launch connection",{76,26},{79,23}},
        Feeder{"America local / desert connection",{-118,36},{-112,34}},
        Feeder{"Europe local / alpine connection",{7,49},{12,46}}}) {
        result=add_infrastructure(*result,InfrastructureKind::skyway,feeder.start);if(!result)return result;
        auto catalogue=infrastructure_parts(*result);if(!catalogue)return std::unexpected(catalogue.error());
        auto p=catalogue->back();p.end=feeder.end;p.name=feeder.name;p.tunnel_class=TunnelSizeClass::local;
        p.terminal_b=true;
        result=edit_infrastructure(*result,p);if(!result)return result;
    }
    // Three close lanes in each corridor, with slightly different bends and
    // heights. They remain independent, ordinary editable tunnel recipes.
    for(auto start:std::array{vng::Vec2{108,32},vng::Vec2{70,24},vng::Vec2{-117,34}})for(unsigned lane=0;lane<3;++lane) {
        start.y+=.10F;
        result=add_infrastructure(*result,InfrastructureKind::skyway,start);if(!result)return result;
        auto catalogue=infrastructure_parts(*result);if(!catalogue)return std::unexpected(catalogue.error());
        auto p=catalogue->back();p.end={start.x+10,start.y-3};p.tunnel_class=TunnelSizeClass::local;
        p.name="Local highway / "+std::to_string(int(start.x))+" / lane "+std::to_string(lane+1);
        p.height=.25F;p.terminal_b=true;
        auto curve=TunnelCurve::create(p,*catalogue,*settings);if(!curve)return std::unexpected(curve.error());
        p.bezier_controls=curve->initial_bezier_controls();p.curve_segments=48;
        result=edit_infrastructure(*result,p);if(!result)return result;
    }
    result=add_infrastructure(*result,InfrastructureKind::skyway,{110,35});if(!result)return result;
    auto catalogue=infrastructure_parts(*result);if(!catalogue)return std::unexpected(catalogue.error());
    result=edit_infrastructure(*result,express_route::recipe(catalogue->back().id));if(!result)return result;
    result->metadata[revision]="bezier-lanes-2";
    return result;
}
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
    state.document.timeline_duration=90;
    state.document.keyframe_names={{0,"Atlantic / homeworld"},{45,"The Americas"},{90,"Pacific / hold"}};
    for(const auto time:{0.F,45.F,90.F}) {
        auto key=project::key_property(state,{instance_id,"rotation"},time,Vec3{0,75+time, -18});
        if(!key)return std::unexpected(key.error());
    }
    state.viewport.mode=project::ViewMode::scene;
    state.viewport.selected_object=instance_id;
    state.viewport.inspected_mesh=blueprint_id;
    auto camera=project::ensure_camera(state,{45,8,3.7F,{},1},"Turntable camera");
    if(!camera)return std::unexpected(camera.error());
    state.viewport.editor_camera=project::evaluate_camera(state,0);
    return state;
}
}
