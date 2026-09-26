#include "tunnel_scene.hpp"
#include "../editor/animation.hpp"
#include "../support/earth_structures.hpp"
#include <cmath>
#include <numbers>

namespace example::tunnel {
namespace {
using namespace vng;
namespace project = editor_example;
namespace vm = content::vmesh;
constexpr f32 tau = 2 * std::numbers::pi_v<f32>;
constexpr f32 degrees = 360 / tau;

// Material and topology come from the same Earth infrastructure recipe. Only
// the sampled route and unit scale differ; there is no bespoke interior tube.
vm::Document document(earth::detail::StructureMesh source,std::string name) {
    vm::Document mesh;
    std::vector<f32> p,n,c,e;
    for(const auto& v:source.vertices) {
        p.insert(p.end(),{v.position.x,v.position.y,v.position.z});
        n.insert(n.end(),{v.normal.x,v.normal.y,v.normal.z});
        c.insert(c.end(),{v.color.x,v.color.y,v.color.z,1});e.push_back(v.emission);
    }
    mesh.vertex_count=source.vertices.size();mesh.faces=std::move(source.faces);
    mesh.vertex_fields={{"position",{vm::ScalarType::Float32,3},std::move(p)},
        {"normal",{vm::ScalarType::Float32,3},std::move(n)},
        {"color/0",{vm::ScalarType::Float32,4},std::move(c)},
        {"emission",{vm::ScalarType::Float32,1},std::move(e)}};
    mesh.metadata={{"name",std::move(name)},{"units","kilometres"},{"render/lighting","tunnel"},
        {"source/tool","examples/scenes/tunnel_scene.cpp"},{"geometry/recipe","earth/tunnel"}};
    return mesh;
}
constexpr f32 tunnel_size=radius/earth::detail::tunnel_inner_height;
vm::Document shell(bool exit_frame) {
    std::vector<earth::detail::TunnelSection> path;
    for(f32 s=-12;;) {
        const auto distance=exit_frame?route_length-s:s;
        const auto angle=distance/bend_radius;
        earth::SkywaySample frame;
        if(exit_frame)frame={exit_center(distance),{0,std::sin(angle),-std::cos(angle)},
            {1,0,0},{0,std::cos(angle),std::sin(angle)}};
        else frame={center(distance),{std::sin(angle),0,-std::cos(angle)},
            {std::cos(angle),0,std::sin(angle)},{0,1,0}};
        path.push_back({frame,tunnel_size});
        if(s==route_length)break;
        const bool detailed=exit_frame?s>route_length-156:s<120;
        s=std::min(route_length,s+(detailed?1.5F:24.F));
    }
    return document(earth::detail::tunnel_shell(path,1),"SKYWAY / local / 6 km hexagonal bore / 1000 km route");
}
vm::Document rib() {
    return document(earth::detail::tunnel_collar({{0,0,0},{0,0,1},{1,0,0},{0,1,0}},tunnel_size),
        "SKYWAY / structural collar");
}
project::CameraPose camera_at(f32 time) {
    const auto s=2.F+time*.18F;
    const auto c=center(s);
    const auto yaw=-s/bend_radius*degrees;
    constexpr f32 focus=300.F;
    return {yaw,0,focus,{c.x-std::sin(yaw/degrees)*focus-.20F,-.28F,
        c.z-std::cos(yaw/degrees)*focus},.82F};
}
}
vng::Vec3 center(vng::f32 distance) {
    const auto a=distance/bend_radius;
    // Stable near the origin, avoiding cancellation in 1-cos(a).
    return {2*bend_radius*std::pow(std::sin(a*.5F),2.F),0,-bend_radius*std::sin(a)};
}
vng::Vec3 exit_center(vng::f32 remaining) {
    const auto c=center(remaining);
    return {0,-c.x,-c.z};
}
vng::content::vmesh::Document shell_mesh(bool exit_frame) {return shell(exit_frame);}
vng::content::vmesh::Document collar_mesh() {return rib();}
vng::content::Result<editor_example::State> author_scene(const std::filesystem::path& assets) {
    auto hull=editor::EditableMesh::create(shell_mesh());
    if(!hull)return std::unexpected(hull.error());
    auto collar=editor::EditableMesh::create(rib());
    if(!collar)return std::unexpected(collar.error());
    auto ship=editor::EditableMesh::load(assets/"spaceship.vmesh");
    if(!ship)return std::unexpected(ship.error());
    auto ship_document=ship->document();
    ship_document.metadata["render/lighting"]="tunnel";
    ship_document.metadata["units"]="kilometres";
    for(auto& field:ship_document.vertex_fields)
        if(field.name=="position")
            for(auto& value:std::get<std::vector<f32>>(field.values))value*=.008F;
    ship=editor::EditableMesh::create(std::move(ship_document));
    if(!ship)return std::unexpected(ship.error());
    project::State state{.document={.mesh=std::move(*hull)}};
    state.document.timeline_duration=duration;
    constexpr auto collar_id=static_cast<project::BlueprintId>(3),ship_id=static_cast<project::BlueprintId>(4);
    state.document.mesh_assets.push_back({collar_id,"SKYWAY / structural collar",std::move(*collar),{}});
    state.document.mesh_assets.push_back({ship_id,"SKYWAY / service craft",std::move(*ship),{}});
    state.document.next_blueprint_id=5;
    state.document.instances={{1,project::BlueprintId::mesh,"Tunnel interior / kilometres",project::MeshSettings{}, {}}};
    state.document.next_instance_id=2;
    for(f32 s=-6;s<120;s+=6.F) {
        const auto id=state.document.next_instance_id++;
        state.document.instances.push_back({id,collar_id,"Collar / km "+std::to_string(s),
            project::MeshSettings{}, {center(s),{0,-s/bend_radius*degrees,0},1}});
    }
    // Tens-of-metres craft make the six-kilometre opening legible as a
    // megastructure. These are ordinary independently editable instances.
    for(unsigned i=0;i<7;++i) {
        const auto id=state.document.next_instance_id++;
        const auto s=3.F+static_cast<f32>(i)*1.75F;
        auto p=center(s);p.x+=(i%2 ? .42F : -.43F);p.y=-.37F+.1F*static_cast<f32>(i%3);
        state.document.instances.push_back({id,ship_id,"Service flight / "+std::to_string(i+1),
            project::MeshSettings{}, {p,{0,-s/bend_radius*degrees,0},i==0 ? 1.8F : 1.F}});
        for (const auto time:{0.F,duration}) {
            auto q=p;q.z-=time*.20F;
            if(auto key=project::key_property(state,{id,"position"},time,q);!key)return std::unexpected(key.error());
        }
    }
    state.document.world_bounds={{-5,-5,-1010},{100,5,20}};
    state.document.environment={.stars=0,.exposure=.95F,.bloom_threshold=1.1F,.bloom_strength=.30F};
    auto camera=project::ensure_camera(state,camera_at(0),"Inside the skyway / slow cruise");
    if(!camera)return std::unexpected(camera.error());
    for(unsigned t=0;t<=static_cast<unsigned>(duration);t+=3)
        if(auto key=project::key_camera(state,*camera,static_cast<f32>(t),camera_at(static_cast<f32>(t)));!key)
            return std::unexpected(key.error());
    state.document.keyframe_names={{0,"01 / A city-sized bore"},{18,"02 / Into the radiance"},{36,"03 / The route continues"}};
    state.viewport.mode=project::ViewMode::scene;
    state.viewport.editor_camera=camera_at(0);
    if(auto valid=project::validate_animation(state);!valid)return std::unexpected(valid.error());
    return state;
}
}
