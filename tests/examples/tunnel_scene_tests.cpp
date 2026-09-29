#include "scenes/tunnel_scene.hpp"
#include "scenes/tunnel_departure.hpp"
#include "editor/animation.hpp"
#include "editor/rotation_math.hpp"
#include "support/earth_assets.hpp"
#include "support/earth_structures.hpp"
#include "support/earth_placement.hpp"
#include "support/mesh_frame.hpp"
#include <vng/content/document.hpp>
#include <vng/editor/limits.hpp>
#include <vng/editor/mesh.hpp>
#include <vng/spatial/triangle_bvh.hpp>
#include <stdlib.h>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numbers>

TEST_CASE("Tunnel bore and collars are regular octagons with readable planar lining", "[example][tunnel]") {
    using namespace vng;
    namespace e=example::earth;
    using namespace e::placement;
    const auto size=3.F/e::detail::tunnel_inner_height;
    const e::SkywaySample frame{{0,0,0},{0,0,1},{1,0,0},{0,1,0}};
    std::vector<e::detail::TunnelSection> path;
    for(unsigned i=0;i<6;++i) {
        auto section=frame;section.position.z=f32(i)*4;
        path.push_back({section,size});
    }
    const auto shell=e::detail::tunnel_shell(path,1);
    const auto collar=e::detail::tunnel_collar(frame,size);
    const auto length=[](Vec3 p){return std::hypot(p.x,p.y,p.z);};
    const auto sub=[](Vec3 a,Vec3 b){return add(a,mul(b,-1));};
    // Independent edge/radius checks catch both stretched profiles and an
    // accidental return to six sides. The first ring is the outer skin.
    const auto check_ring=[&](const auto& mesh,std::size_t first) {
        const auto edge=length(sub(mesh.vertices[first].position,mesh.vertices[first+1].position));
        const auto radius=std::hypot(mesh.vertices[first].position.x,mesh.vertices[first].position.y);
        for(unsigned j=0;j<8;++j) {
            const auto a=mesh.vertices[first+j].position,b=mesh.vertices[first+(j+1)%8].position;
            CHECK(std::abs(length(sub(a,b))-edge)<.00001F);
            CHECK(std::abs(std::hypot(a.x,a.y)-radius)<.00001F);
        }
    };
    check_ring(shell,0);
    REQUIRE(collar.vertices.size()==32);
    for(unsigned ring=0;ring<4;++ring)check_ring(collar,ring*8);
    const auto lining_first=path.size()*8;
    for(unsigned j=0;j<8;++j) {
        const auto& a=shell.vertices[lining_first+j*2];
        const auto& b=shell.vertices[lining_first+j*2+1];
        const auto& next=shell.vertices[lining_first+((j+1)%8)*2];
        CHECK(a.normal==b.normal);
        CHECK(a.normal!=next.normal); // A crease, not cylinder-like smoothing.
        CHECK(std::abs(dot(a.normal,sub(b.position,a.position)))<.00001F);
        CHECK(std::abs(length(a.normal)-1)<.00001F);
        CHECK(a.color!=next.color);
        CHECK(a.color!=shell.vertices[lining_first+16+j*2].color); // Bay variation.
        CHECK(std::max(std::abs(a.position.x),std::abs(a.position.y))==3.F);
    }
    for(const auto* mesh:{&shell,&collar})for(const auto& f:mesh->faces) {
        const auto& a=mesh->vertices[f.vertices[0]];
        const auto& b=mesh->vertices[f.vertices[1]];
        const auto& c=mesh->vertices[f.vertices[2]];
        CHECK(dot(cross(sub(b.position,a.position),sub(c.position,a.position)),a.normal)>0);
    }
    const auto unlit=e::detail::tunnel_shell(path,0);
    REQUIRE(unlit.vertices.size()==shell.vertices.size());
    CHECK(unlit.faces==shell.faces);
    for(std::size_t i=0;i<shell.vertices.size();++i) {
        CHECK(unlit.vertices[i].position==shell.vertices[i].position);
        CHECK(unlit.vertices[i].normal==shell.vertices[i].normal);
        CHECK(unlit.vertices[i].emission==0);
    }
    // Detail remains indexed and linear in route samples, not a box per panel.
    CHECK(shell.vertices.size()<=50*path.size()+80);
}

TEST_CASE("Collars sit on route samples, where the lip meets the octagonal lining", "[example][tunnel]") {
    namespace e=example::earth;
    CHECK(e::detail::collar_sections(24)==std::array<std::size_t,7>{0,4,8,12,16,20,24});
    CHECK(e::detail::collar_sections(192)==std::array<std::size_t,7>{0,32,64,96,128,160,192});
    for(std::size_t segments:{8U,32U,40U,64U,256U}) {
        const auto sections=e::detail::collar_sections(segments);
        CHECK(sections.front()==0);
        CHECK(sections.back()==segments);
        for(std::size_t i=1;i<sections.size();++i) {
            CHECK(sections[i]>sections[i-1]);
            CHECK(std::abs(double(sections[i])-double(i*segments)/6)<=.5);
        }
    }
}

TEST_CASE("One shared tunnel has a dark outside, lit lining and open ends", "[example][tunnel]") {
    using namespace vng;
    namespace e=example::earth;
    const auto size=2.F/e::detail::tunnel_inner_height;
    const std::array path{e::detail::TunnelSection{{{0,0,0},{0,0,1},{1,0,0},{0,1,0}},size},
        e::detail::TunnelSection{{{0,0,10},{0,0,1},{1,0,0},{0,1,0}},size}};
    const auto mesh=e::detail::tunnel_shell(path,1);
    std::vector<Vec3> positions;std::vector<spatial::TriangleBvh::Triangle> triangles;
    for(const auto& v:mesh.vertices) {
        positions.push_back(v.position);
        CHECK(std::isfinite(v.normal.x+v.normal.y+v.normal.z));
    }
    for(const auto& f:mesh.faces)triangles.push_back(f.vertices);
    spatial::TriangleBvh picking{positions,triangles};
    const auto inside=picking.intersect({{0,0,5},{1,0,0}});
    const auto outside=picking.intersect({{5,0,5},{-1,0,0}});
    REQUIRE(inside);REQUIRE(outside);
    const auto& lining=mesh.vertices[mesh.faces[inside->triangle].vertices[0]];
    const auto& hull=mesh.vertices[mesh.faces[outside->triangle].vertices[0]];
    CHECK(lining.normal.x<0.F);CHECK(hull.normal.x>0.F);
    CHECK(hull.emission==0.F);
    CHECK(std::ranges::any_of(mesh.vertices,[](const auto& v){return v.emission>0.F;}));
    CHECK(lining.color!=hull.color);
    CHECK(inside->distance<5-outside->distance); // Real wall thickness.
    CHECK_FALSE(picking.intersect({{0,0,-1},{0,0,1}}));
    CHECK_FALSE(picking.intersect({{0,0,11},{0,0,-1}}));
    CHECK(e::detail::tunnel_shell({},1).vertices.empty());
}

TEST_CASE("Tunnel is a kilometre-scale editable scene with an occluded exit", "[example][tunnel]") {
    using namespace vng;
    namespace tunnel=example::tunnel;
    auto scene=tunnel::author_scene(VNG_TUNNEL_ASSETS);
    REQUIRE(scene);
    REQUIRE(editor_example::validate_animation(*scene));
    CHECK(tunnel::radius*2==6.F);
    f32 low=0,high=0;
    for(u32 i=0;i<scene->document.mesh.size();++i) {
        const auto y=scene->document.mesh.position(i).y;
        low=std::min(low,y);high=std::max(high,y);
    }
    // Six kilometres of clear interior height, plus wall and crown thickness.
    CHECK(high-low>6.F);
    CHECK(high-low<6.3F);
    CHECK(scene->document.mesh.document().metadata.at("geometry/recipe")=="earth/tunnel");
    // The close-up uses exactly the original Earth octagonal structural rib.
    const auto& collar=scene->document.mesh_assets.front().geometry;
    const auto shared=example::earth::detail::tunnel_collar({{0,0,0},{0,0,1},{1,0,0},{0,1,0}},
        tunnel::radius/example::earth::detail::tunnel_inner_height);
    REQUIRE(collar.size()==shared.vertices.size());
    for(u32 i=0;i<collar.size();++i) {
        const auto p=collar.position(i);
        CHECK(p==shared.vertices[i].position);
    }
    CHECK(tunnel::route_length==1000.F);
    CHECK(scene->document.environment.stars==0);
    CHECK(scene->document.environment.bloom_strength>0.F);
    CHECK(scene->document.mesh.document().metadata.at("render/lighting")=="tunnel");
    CHECK(scene->document.mesh_assets.size()==2);
    CHECK(scene->document.instances.size()>25);
    CHECK(editor_example::has_camera(*scene));
    // Even a ray aimed directly at the remote portal exits the bore before
    // the midpoint of this Earth-curved route. No opaque "fake exit" disk.
    const auto end=tunnel::center(tunnel::route_length);
    const auto middle=tunnel::center(tunnel::route_length*.5F);
    CHECK(std::hypot(end.x*.5F-middle.x,end.z*.5F-middle.z)>tunnel::radius);
    for(f32 t=0;t<=tunnel::duration;t+=.5F) {
        const auto pose=editor_example::evaluate_camera(*scene,t);
        REQUIRE(editor_example::valid_camera_pose(pose));
        const auto snapshot=editor_example::camera(pose,editor_example::ViewMode::scene).snapshot({1280,800});
        REQUIRE(snapshot);
        const auto eye=snapshot->position;
        CHECK(std::abs(eye.y)<tunnel::radius*.5F);
        CHECK(eye.z<0.F);
        CHECK(eye.z>-10.F);
    }
    auto encoded=editor_example::encode(*scene);
    REQUIRE(encoded);
    auto restored=editor_example::decode(*encoded);
    REQUIRE(restored);
    CHECK(restored->document.mesh.document()==scene->document.mesh.document());
    CHECK(restored->document.instances==scene->document.instances);
    CHECK(restored->document.environment==scene->document.environment);
    CHECK(editor_example::evaluate_camera(*restored,9)==editor_example::evaluate_camera(*scene,9));
}

TEST_CASE("Express departure has a continuous close camera and clears the low terminal", "[example][tunnel][departure]") {
    using namespace vng;
    namespace d=example::tunnel::departure;
    namespace p=editor_example;
    auto scene=d::author_scene(VNG_TUNNEL_ASSETS);
    INFO((scene ? "authored departure" : scene.error().message));REQUIRE(scene);
    REQUIRE(p::validate_animation(*scene));REQUIRE(p::validate_active_cameras(*scene));
    // The skyway's five blueprints first, then the voyage's bodies,
    // installations, belt and effects.
    REQUIRE(scene->document.mesh_assets.size()>5);
    CHECK(scene->document.mesh_assets[4].name=="MANTA / shuttle");
    for(const std::string name:{"MOON / Selene","LUNAR / Serenity mining works","GATEWAY / Arabian orbital yard",
                                 "GATEWAY / habitat ring","BELT / basalt","WARP / streak"})
        CHECK(std::ranges::count(scene->document.mesh_assets,name,&p::MeshBlueprint::name)==1);
    CHECK(scene->document.timeline_duration==example::tunnel::voyage::duration);
    CHECK(std::ranges::count_if(scene->document.instances,[](const auto& i){return std::holds_alternative<p::CameraSettings>(i.settings);})==1);
    const auto* planet=p::instance_mesh(*scene,d::earth);REQUIRE(planet);
    const auto saved_planet=content::vmesh::read_vmesh(std::filesystem::path(VNG_TUNNEL_ASSETS)/"earth_future.vmesh");REQUIRE(saved_planet);
    CHECK(planet->document().vertex_fields==saved_planet->vertex_fields);
    CHECK(planet->document().faces==saved_planet->faces); // The actual Earth route, not a duplicate shell.
    const auto infrastructure=example::earth::infrastructure_settings(planet->document());REQUIRE(infrastructure);
    CHECK(infrastructure->night_lights);CHECK(infrastructure->skyways);CHECK(infrastructure->launch_hubs);
    const auto parts=example::earth::infrastructure_parts(planet->document());REQUIRE(parts);CHECK(parts->size()>70);
    CHECK(std::ranges::count(*parts,example::earth::InfrastructureKind::skyway,&example::earth::InfrastructurePart::kind)>=36);
    CHECK(std::ranges::count_if(*parts,[](const auto& part){
        return part.kind==example::earth::InfrastructureKind::hub&&part.location.x>49&&part.location.x<57&&part.location.y>19&&part.location.y<27;
    })>=5);
    CHECK(-d::earth_center.y-d::earth_radius<200.F);
    CHECK_FALSE(p::find_instance(*scene,d::terminal));
    CHECK_FALSE(p::find_instance(*scene,1)); // No separate cinematic tunnel instance.
    const auto express=std::ranges::find(*parts,example::earth::express_route::name,&example::earth::InfrastructurePart::name);
    REQUIRE(express!=parts->end());CHECK(express->tunnel_class==example::earth::TunnelSizeClass::local);
    CHECK(express->bezier_controls.has_value());CHECK(express->terminal_b);
    CHECK(express->end.x>50);CHECK(express->end.x<54);
    CHECK(express->end.y>20);CHECK(express->end.y<24);
    CHECK(express->terminal_incline==8);
    const auto* ship=p::find_instance(*scene,d::hero);REQUIRE(ship);
    const auto* earth_instance=p::find_instance(*scene,d::earth);REQUIRE(earth_instance);
    const auto planet_inverse=example::mesh_frame::inverse(p::mesh_transform(p::ViewMode::scene,earth_instance->transform));REQUIRE(planet_inverse);
    // Around Earth, until the cut to the Moon; later locations are staged
    // near the origin and Earth moves away.
    for(f32 t=0;t<example::tunnel::voyage::moon_cut;t+=.25F) {
        CAPTURE(t);
        const auto a=p::evaluate_instance(*scene,*ship,t).transform.position;
        const auto b=p::evaluate_instance(*scene,*ship,t+.25F).transform.position;
        const auto local_a=example::mesh_frame::point(*planet_inverse,a),local_b=example::mesh_frame::point(*planet_inverse,b);
        const auto obstruction=planet->picking_index().intersect({{local_a.x,local_a.y,local_a.z},
            {local_b.x-local_a.x,local_b.y-local_a.y,local_b.z-local_a.z},0,1});
        if(obstruction) {
            const auto& mesh=planet->document();
            const auto owner=std::ranges::find(mesh.vertex_fields,example::earth::infrastructure_part_field,&content::vmesh::VertexField::name);
            REQUIRE(owner!=mesh.vertex_fields.end());
            const auto id=std::get<std::vector<u32>>(owner->values)[mesh.faces[obstruction->triangle].vertices[0]];
            const auto part=std::ranges::find(*parts,id,&example::earth::InfrastructurePart::id);
            INFO("Flight obstruction: "<<(part==parts->end()?"Terrain/clouds":part->name));
            CHECK_FALSE(obstruction);
        }
        CHECK(std::hypot(a.x-d::earth_center.x,a.y-d::earth_center.y,a.z-d::earth_center.z)>d::earth_radius+70.F);
        const auto pose=p::evaluate_camera(*scene,t);
        REQUIRE(p::valid_camera_pose(pose));
        auto view=p::camera(pose,p::ViewMode::scene).snapshot({1280,800});REQUIRE(view);
        const auto eye=view->position;
        // Camera moves must not put it inside a wall or block its view
        // from its hero by the shell or the three-lane terminal dividers.
        const auto local_eye=example::mesh_frame::point(*planet_inverse,eye);
        CHECK_FALSE(planet->picking_index().intersect({{local_eye.x,local_eye.y,local_eye.z},
            {local_a.x-local_eye.x,local_a.y-local_eye.y,local_a.z-local_eye.z},0,.98}));
    }
    unsigned overtaken{};
    std::vector<const p::SceneInstance*> traffic;
    for(const auto& instance:scene->document.instances) {
        // The skyway's own traffic; the voyage's craft are added after the camera.
        if(static_cast<u32>(instance.blueprint)<7 || static_cast<u32>(instance.blueprint)>9 || instance.id>d::camera_id)continue;
        CHECK(std::get<p::MeshSettings>(p::evaluate_instance(*scene,instance,0).settings).visible);
        traffic.push_back(&instance);
        const auto start=p::evaluate_instance(*scene,instance,0).transform.position;
        const auto later=p::evaluate_instance(*scene,instance,24).transform.position;
        overtaken+=start.z<d::flight(0).z && later.z>d::flight(24).z;
    }
    CHECK(overtaken>=10);
    CHECK(d::start_distance>140.F);
    CHECK(std::abs(d::speed(0)-2.2F*1.3F*1.2F)<.00001F);
    CHECK(std::abs(d::travel(0))<.00001F);
    for(auto t:{0.F,6.F,16.F,26.F,36.F}) {
        const auto u=(t+2.F)/40.F;
        const auto previous_acceleration=12.F/40.F*6.F*u*(1.F-u);
        CHECK(std::abs((d::speed(t+.01F)-d::speed(t-.01F))/.02F-1.4F*previous_acceleration)<.002F);
    }
    CHECK(d::speed(.1F)>d::speed(0)+.01F); // No initial acceleration delay.
    // Inside the tube: the catch-up from the opening into the chase, and one
    // eased move before the throat.
    CHECK(d::courier_arrival<d::catch_up_begin);CHECK(d::catch_up_end<d::exit_approach);
    CHECK(d::exit_approach+d::exit_approach_duration<d::throat_time);
    const auto speed_at=[](f32 time){const auto v=d::velocity(time);return std::hypot(v.x,v.y,v.z);};
    for(f32 t=.25F;t<d::handoff_time;t+=.25F) {
        CHECK(speed_at(t)>=d::initial_speed);
        CHECK(std::abs(d::flight(t).x)<.02F); // Only Earth-space floating-point roundoff, no lane weaving.
    }
    const auto acceleration=[&](f32 t){return (speed_at(t+.1F)-speed_at(t-.1F))/.2F;};
    CHECK(std::abs(acceleration(65))<.02F);
    CHECK(std::abs(acceleration(71.5F))<.02F);
    const auto mouth_velocity=d::velocity(d::exit_time);
    CHECK(std::abs(std::atan2(mouth_velocity.y,-mouth_velocity.z)*180/std::numbers::pi_v<f32>-8)<1.F);
    // Continuous through the throat and the mouth: the heading holds, and the
    // speed changes only as fast as the exit surge accelerates it.
    for(auto t:{d::throat_time,d::exit_time}) {
        const auto a=d::velocity(t-.001F),b=d::velocity(t+.001F);
        const double la=std::hypot(a.x,a.y,a.z),lb=std::hypot(b.x,b.y,b.z);
        CHECK((double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z)/(la*lb)>std::cos(.001));
        CHECK(std::abs(lb-la)<std::abs(d::speed(t+.001F)-d::speed(t-.001F))+.001*la);
    }
    // Nearing the exit the courier surges to about ten times its speed.
    CHECK(d::speed(d::exit_time)>9.F*d::speed(d::surge_begin));
    CHECK(d::surge_begin<d::throat_time);
    const auto camera_eye=[&](f32 t){
        const auto* camera=p::active_camera(*scene,t);REQUIRE(camera);CHECK(camera->id==d::camera_id);
        return p::evaluate_instance(*scene,*camera,t).transform.position;
    };
    const auto relative_eye=[&](f32 t){auto a=camera_eye(t),b=d::flight(t);return Vec3{a.x-b.x,a.y-b.y,a.z-b.z};};
    const auto distance=[](Vec3 a,Vec3 b){return std::hypot(a.x-b.x,a.y-b.y,a.z-b.z);};
    const auto on_screen=[&](const p::SceneInstance& instance,f32 t) {
        auto snapshot=p::camera(p::evaluate_camera(*scene,t),p::ViewMode::scene).snapshot({1280,800});REQUIRE(snapshot);
        auto position=p::evaluate_instance(*scene,instance,t).transform.position;
        const std::array<f32,4> world{position.x,position.y,position.z,1};
        std::array<f32,4> clip{};
        for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)clip[row]+=snapshot->view_projection[col][row]*world[col];
        return clip[3]>0&&std::abs(clip[0])<clip[3]&&std::abs(clip[1])<clip[3];
    };
    // The opening: the camera waits in the empty bore and drifts a little.
    // Traffic comes in, then the courier, far faster; a moment later the
    // camera races after it into the chase.
    const auto drift=distance(camera_eye(0),camera_eye(d::catch_up_begin));
    CHECK(drift>.05F);CHECK(drift<.5F);
    CHECK_FALSE(on_screen(*ship,0));
    unsigned arrivals{};
    for(const auto* craft:traffic) {
        const auto a=p::evaluate_instance(*scene,*craft,0).transform.position;
        if(distance(a,camera_eye(0))>24.F)continue; // beyond the haze: the bore reads empty
        ++arrivals;
        CHECK_FALSE(on_screen(*craft,0));
        CHECK(on_screen(*craft,d::courier_arrival));
        const auto b=p::evaluate_instance(*scene,*craft,1).transform.position;
        CHECK(distance(a,b)*10<d::speed(d::courier_arrival));
    }
    CHECK(arrivals==3);
    CHECK_FALSE(on_screen(*ship,d::courier_arrival));
    for(f32 t=d::courier_arrival+.2F;t<d::catch_up_end+.5F;t+=.05F)CHECK(on_screen(*ship,t));
    // The catch-up is swift, not a teleport: kilometres in half a second,
    // frame by frame, easing into the chase.
    CHECK(d::catch_up_end-d::catch_up_begin>.4F);
    CHECK(distance(relative_eye(d::catch_up_begin),{})>2.F);
    for(f32 t=d::catch_up_begin;t<d::catch_up_end;t+=1.F/60)
        CHECK(distance(camera_eye(t),camera_eye(t+1.F/60))<.4F);
    CHECK(std::abs(distance(relative_eye(d::catch_up_end),{})-.7F*std::hypot(.1F,.14F,.48F))<.001F);
    // The move for the exit: continuous, quick but bounded, around the ship.
    const auto begin=d::exit_approach,end=begin+d::exit_approach_duration;
    CHECK(distance(relative_eye(begin-.001F),relative_eye(begin+.001F))<.005F);
    CHECK(distance(relative_eye(end-.001F),relative_eye(end+.001F))<.005F);
    CHECK(distance(relative_eye(begin),relative_eye(end))>.03F);
    auto previous=relative_eye(begin);
    for(f32 t=begin+.02F;t<end;t+=.02F) {
        const auto current=relative_eye(t);
        CHECK(distance(previous,current)<.01F);
        CHECK(distance(current,{})>.15F);
        previous=current;
    }
    CHECK(distance(relative_eye(end),{})<distance(relative_eye(8),{})*.85F);
    CHECK(relative_eye(end).x>relative_eye(8).x+.05F);
    const auto encoded=p::encode(*scene);REQUIRE(encoded);
    const auto restored=p::decode(*encoded);REQUIRE(restored);
    CHECK(p::active_camera(*restored,40)->id==d::camera_id);
    CHECK(p::evaluate_camera(*restored,40)==p::evaluate_camera(*scene,40));
}

TEST_CASE("A full-budget Earth still loads, saves beside its draft and fits the departure",
          "[example][earth][limits][departure]") {
    using namespace vng;
    namespace e=example::earth;
    namespace p=editor_example;
    const std::filesystem::path assets=VNG_TUNNEL_ASSETS;
    auto earth=editor::EditableMesh::load(assets/"earth_future.vmesh");REQUIRE(earth);
    // Grow the committed Earth to its budget by repeating its first vertices
    // (terrain: the most faces, so the most text, per vertex) and the faces
    // among them.
    auto grown=earth->document();
    const auto base=grown.vertex_count;
    REQUIRE(base<e::max_earth_vertices);
    const auto extra=e::max_earth_vertices-base;
    for(auto& field:grown.vertex_fields)std::visit([&](auto& values) {
        const auto prefix=std::vector(values.begin(),values.begin()+std::ptrdiff_t(extra*field.type.components));
        values.insert(values.end(),prefix.begin(),prefix.end());
    },field.values);
    const auto faces=grown.faces.size();
    for(std::size_t i=0;i<faces;++i) {
        const auto v=grown.faces[i].vertices;
        if(v[0]<extra&&v[1]<extra&&v[2]<extra)
            grown.faces.emplace_back(u32(v[0]+base),u32(v[1]+base),u32(v[2]+base));
    }
    grown.vertex_count=e::max_earth_vertices;
    // It still loads as an editor mesh (32 MiB of .vmesh text).
    const auto text=content::vmesh::write_vmesh(grown,{.limits=editor::mesh_limits()});REQUIRE(text);
    auto full=editor::EditableMesh::create(grown);REQUIRE(full);
    // Its scene can hold it twice while a blueprint edit is unapplied.
    auto cube=editor::EditableMesh::load(assets/"colored_cube.vmesh");REQUIRE(cube);
    p::State scene{.document={.mesh=std::move(*cube)}};
    scene.document.mesh_assets.push_back({static_cast<p::BlueprintId>(3),"EARTH / full budget",*full,{}});
    scene.document.next_blueprint_id=4;
    scene.document.mesh_drafts.emplace(static_cast<p::BlueprintId>(3),*full);
    CHECK(p::encode_scene(scene));
    // The departure regenerated from it still saves and reopens.
    std::array<char,64> name{};
    std::strcpy(name.data(),"/tmp/vng-full-earth-XXXXXX");
    REQUIRE(::mkdtemp(name.data()));
    const std::filesystem::path folder=name.data();
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{folder};
    for(const auto* asset:{"colored_cube.vmesh","spaceship.vmesh","fleet_carrier.vmesh","fleet_frigate.vmesh","fleet_escort.vmesh"})
        std::filesystem::create_symlink(assets/asset,folder/asset);
    {
        std::ofstream out(folder/"earth_future.vmesh",std::ios::binary);
        out<<*text;
        REQUIRE(out);
    }
    const auto departure=example::tunnel::departure::author_scene(folder);REQUIRE(departure);
    CHECK(p::encode_scene(*departure));
}

TEST_CASE("The voyage flies nose first, keeps its courier in frame and never shakes", "[example][tunnel][departure]") {
    using namespace vng;
    namespace d=example::tunnel::departure;namespace v=example::tunnel::voyage;namespace p=editor_example;
    namespace r=editor_example::rotation_math;
    auto scene=d::author_scene(VNG_TUNNEL_ASSETS);REQUIRE(scene);
    const auto* ship=p::find_instance(*scene,d::hero);REQUIRE(ship);
    const auto transform=[&](const p::SceneInstance& instance,f32 t){return p::evaluate_instance(*scene,instance,t).transform;};
    const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    const auto unit=[](Vec3 a){const auto l=std::hypot(a.x,a.y,a.z);return Vec3{a.x/l,a.y/l,a.z/l};};
    const auto velocity=[&](const p::SceneInstance& instance,f32 t,f32 h){
        const auto a=transform(instance,t-h).position,b=transform(instance,t+h).position;
        return Vec3{(b.x-a.x)/(2*h),(b.y-a.y)/(2*h),(b.z-a.z)/(2*h)};
    };
    // Craft point their local -Z along their motion. Headings along the
    // scene's X axis put Euler angles in gimbal lock; the fleet flew
    // backwards and upside down once when that was mishandled.
    for(f32 t=d::handoff_time+.5F;t<v::duration-5;t+=.25F) {
        if(std::abs(t-v::moon_cut)<.3F||std::abs(t-v::belt_cut)<.3F)continue;
        const auto motion=velocity(*ship,t,.02F);
        if(std::hypot(motion.x,motion.y,motion.z)<.05F)continue; // holding in the clearing
        CAPTURE(t);
        CHECK(dot(unit(motion),r::direction(transform(*ship,t).rotation,{0,0,-1}))>.9F);
    }
    unsigned fleet{},traffic{};
    for(const auto& instance:scene->document.instances) {
        const std::string_view name=instance.name;
        const bool escort=name.starts_with("BASTION / ")&&!name.contains("construction");
        const bool in_fleet=escort||name.starts_with("LANCER / ")||name.starts_with("MANTA / ")||name.starts_with("KESTREL / wing");
        const bool gateway=name=="Freighter / inbound"||name=="Shuttle / outbound"||name=="Tug / pod racks"||name=="Patrol / picket";
        if(!in_fleet&&!gateway)continue;
        CAPTURE(name);
        const auto t=in_fleet ? 105.F : 40.F; // cruising in formation, before the jumps
        const auto rotation=transform(instance,t).rotation;
        CHECK(dot(unit(velocity(instance,t,.5F)),r::direction(rotation,{0,0,-1}))>.99F);
        CHECK(r::direction(rotation,{0,1,0}).y>.5F);
        fleet+=in_fleet;traffic+=gateway;
    }
    CHECK(fleet==11);CHECK(traffic==4);
    // The base is built on the Moon: they share one transform everywhere.
    const auto named=[&](std::string_view name){
        const auto found=std::ranges::find(scene->document.instances,name,&p::SceneInstance::name);
        REQUIRE(found!=scene->document.instances.end());return &*found;
    };
    const auto* moon=named("MOON / Selene");const auto* base=named("LUNAR / Serenity mining works");
    for(auto t:{0.F,40.F,57.F,100.F}) CHECK(transform(*moon,t)==transform(*base,t));
    // The courier's position in pixels on a 1280x800 frame, as played back.
    const auto pixel=[&](f32 t) {
        auto snapshot=p::render_camera(*scene,t).snapshot({1280,800});REQUIRE(snapshot);
        const auto position=transform(*ship,t).position;
        const std::array<f32,4> world{position.x,position.y,position.z,1};
        std::array<f32,4> clip{};
        for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)clip[row]+=snapshot->view_projection[col][row]*world[col];
        REQUIRE(clip[3]>0);
        return std::array{(clip[0]/clip[3]+1)*640,(1-clip[1]/clip[3])*400};
    };
    // In frame through every act, apart from the shots where it has yet to
    // arrive or has already gone.
    for(auto t:{23.F,27.F,36.F,43.F,45.F,47.5F,52.F,54.F,61.F,63.F,68.F,71.F,74.F,77.F,80.F,84.F,
                90.F,92.F,99.F,102.F,106.F,110.2F}) {
        CAPTURE(t);
        const auto [x,y]=pixel(t);
        CHECK(x>0.F);CHECK(x<1280.F);CHECK(y>0.F);CHECK(y<800.F);
    }
    // No shake: between cuts, the courier's screen motion is smooth frame to
    // frame at 60 fps: from the tunnel exit through the climb, through the
    // sunrise, weaving through the belt, and taking station in formation.
    // Deliberate camera moves stay under this bound.
    for(const auto& [begin,end]:{std::pair{d::exit_time-1,29.9F},std::pair{64.4F,72.4F},std::pair{75.F,78.5F},
                                 std::pair{80.9F,85.85F},std::pair{97.7F,99.8F},std::pair{100.F,110.4F}}) {
        auto before=pixel(begin),current=pixel(begin+1.F/60);
        for(auto t=begin+2.F/60;t<end;t+=1.F/60) {
            const auto next=pixel(t);
            CAPTURE(t);
            CHECK(std::hypot(next[0]-2*current[0]+before[0],next[1]-2*current[1]+before[1])<3.5F);
            before=current;current=next;
        }
    }
}
