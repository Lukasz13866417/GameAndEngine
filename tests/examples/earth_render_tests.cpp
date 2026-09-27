#include "scenes/earth_scene.hpp"
#include "support/earth_assets.hpp"
#include "support/earth_placement.hpp"
#include "support/earth_connections.hpp"
#include "support/presentation.hpp"
#include "editor/runtime.hpp"
#include "editor/animation.hpp"
#include "editor/scene_file.hpp"
#include "editor/mesh_shading.hpp"
#include "../support/glfw_opengl.hpp"
#include <vng/analysis/manifest.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <iostream>
#include <algorithm>

TEST_CASE("Connected Earth has one visible planet and infrastructure on every side", "[earth][opengl][global-network]") {
    using namespace vng;namespace project=editor_example;namespace earth=example::earth;
    project::SceneFile file;auto state=file.load(std::filesystem::path(VNG_EARTH_ASSETS)/"earth_future.vscene");REQUIRE(state);
    unsigned visible_planets{};
    for(const auto& instance:state->document.instances)
        if(instance.blueprint==earth::blueprint_id)
            if(const auto* mesh=std::get_if<project::MeshSettings>(&instance.settings);mesh&&mesh->visible)++visible_planets;
    CHECK(visible_planets==1); // Co-located spare used to intersect the entire surface.
    state->viewport.mode=project::ViewMode::scene;
    constexpr Extent2D extent{900,900};
    auto window=test::create_hidden_opengl_window(extent.width,extent.height,"Earth global network");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current();REQUIRE(token);auto device=opengl::Device::create(*token);REQUIRE(device);
    auto runtime=project::Runtime::create(*device,*state);REQUIRE(runtime);
    char folder[]="/tmp/vng-earth-global-render-XXXXXX";REQUIRE(::mkdtemp(folder));
    const auto capture=[&](const std::string& name,const gfx::Camera& camera) {
        auto image=runtime->render(*device,{*state,camera,extent,0,false});REQUIRE(image);
        REQUIRE(example::write_rgba8_png(std::filesystem::path(folder)/(name+".png"),extent,
            {reinterpret_cast<const u8*>(image->pixels.data()),image->pixels.size()}));
        return image->pixels;
    };
    const auto orbit=project::camera(project::evaluate_camera(*state,0),project::ViewMode::scene);
    const auto clean=capture("single-planet",orbit);
    // Reproduce the original overlap. Hiding the spare must restore the same
    // render, not just cover the artifact with bloom or a depth bias.
    const auto original=std::ranges::find(state->document.instances,earth::instance_id,&project::SceneInstance::id);
    REQUIRE(original!=state->document.instances.end());
    auto spare=*original;spare.id=state->document.next_instance_id++;spare.transform.rotation={};
    state->document.instances.push_back(spare);++state->document.revision;
    CHECK((capture("overlapping-planets",orbit)!=clean));
    state->document.instances.pop_back();++state->document.revision;
    CHECK((capture("restored-single-planet",orbit)==clean));
    state->viewport.mode=project::ViewMode::mesh;state->viewport.inspected_mesh=earth::blueprint_id;
    state->document.environment.stars=0;
    for(const Vec2 location:std::array<Vec2,8>{{{-150,-25},{-90,25},{-30,-25},{30,25},{90,-25},{150,25},{0,89},{0,-89}}}) {
        gfx::Camera camera;
        camera.set_position(earth::placement::mul(earth::placement::direction(location),3.2F)).look_at({});
        capture("orbit-"+std::to_string(int(location.x))+"-"+std::to_string(int(location.y)),camera);
    }
    // The same layered planet at kilometre scale must not acquire stripes.
    state->viewport.mode=project::ViewMode::scene;state->document.environment.bloom_strength=0;
    gfx::Camera scaled_camera;scaled_camera.set_position({3.2F,0,0}).look_at({});
    scaled_camera.set_perspective({.near_plane=.05F,.far_plane=100000});
    const auto small=capture("unit-earth",scaled_camera);
    project::find_instance(*state,earth::instance_id)->transform.scale=6371;++state->document.revision;
    scaled_camera.set_position({3.2F*6371,0,0}).look_at({});
    const auto large=capture("kilometre-earth",scaled_camera);
    std::size_t changed{};
    for(std::size_t i=0;i<small.size();i+=4) {
        bool different{};
        for(unsigned c=0;c<3;++c)different|=std::abs(std::to_integer<int>(small[i+c])-std::to_integer<int>(large[i+c]))>12;
        changed+=different;
    }
    CHECK(changed<extent.width*extent.height/100);
    std::cout<<"Earth global-network captures: "<<folder<<'\n';
}

TEST_CASE("Earth dispersal terminals and orbital elevators render as editable mesh geometry", "[earth][opengl][structures]") {
    using namespace vng;namespace project=editor_example;namespace earth=example::earth;
    constexpr Extent2D extent{1100,850};
    auto window=test::create_hidden_opengl_window(extent.width,extent.height,"Earth spaceport structures");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current();REQUIRE(token);auto device=opengl::Device::create(*token);REQUIRE(device);
    auto state=earth::author_scene(VNG_EARTH_ASSETS);REQUIRE(state);
    state->viewport.mode=project::ViewMode::mesh;state->document.environment.stars=0;
    const auto base=earth::make_mesh({.visible=false});
    auto terminal=earth::add_infrastructure(base,earth::InfrastructureKind::terminal,{0,0});REQUIRE(terminal);
    *project::mesh_geometry(*state,earth::blueprint_id)=*editor::EditableMesh::create(*terminal);
    auto runtime=project::Runtime::create(*device,*state);REQUIRE(runtime);
    char folder[]="/tmp/vng-spaceport-render-XXXXXX";REQUIRE(::mkdtemp(folder));
    const auto frame=earth::placement::frame({0,0});
    gfx::Camera camera;
    const auto look=[&](Vec3 eye,Vec3 target){camera.set_position(example::mesh_frame::point(frame,eye)).look_at(example::mesh_frame::point(frame,target));};
    const auto render=[&](std::string name,const content::vmesh::Document& mesh,bool diagnostic=false) {
        INFO("Rendering evidence: "<<name);
        *project::mesh_geometry(*state,earth::blueprint_id)=*editor::EditableMesh::create(mesh);++state->document.revision;
        REQUIRE(runtime->update_mesh(*device,*state));
        auto pixels=runtime->render(*device,project::RenderRequest{*state,camera,extent,0,diagnostic});REQUIRE(pixels);
        REQUIRE(example::write_rgba8_png(std::filesystem::path{folder}/(name+".png"),extent,
            {reinterpret_cast<const u8*>(pixels->pixels.data()),pixels->pixels.size()}));
        if(!diagnostic)CHECK(runtime->stats().last_mesh_draw_calls==1);
        return pixels->pixels;
    };
    look({.13F,1.19F,.19F},{0,1.043F,.018F});
    const auto a=render("terminal",*terminal);
    look({0,1.075F,.21F},{0,1.034F,0});render("terminal-mouth",*terminal);
    auto lifted_recipe=earth::infrastructure_parts(*terminal)->front();lifted_recipe.altitude=.12F;lifted_recipe.scale=1.4F;
    auto lifted=earth::edit_infrastructure(*terminal,lifted_recipe);REQUIRE(lifted);
    look({.19F,1.40F,.32F},{0,1.15F,.01F});const auto supported=render("terminal-raised-supports",*lifted);
    const auto stable_uploads=runtime->stats().full_mesh_uploads;
    lifted_recipe.altitude=.16F;auto higher_terminal=earth::edit_infrastructure(*lifted,lifted_recipe);REQUIRE(higher_terminal);
    render("terminal-raised-higher",*higher_terminal);CHECK(runtime->stats().full_mesh_uploads==stable_uploads);
    lifted_recipe.altitude=.12F;lifted_recipe.scaffold=false;
    auto floating=earth::edit_infrastructure(*lifted,lifted_recipe);REQUIRE(floating);
    CHECK(render("terminal-raised-no-supports",*floating)!=supported);
    render("terminal-raised-diagnostic",*floating,true);
    REQUIRE(runtime->diagnostic_manifest());CHECK(runtime->diagnostic_manifest()->items()[0].primitives->size()==floating->faces.size());
    auto elevator=earth::add_infrastructure(base,earth::InfrastructureKind::elevator,{0,0});REQUIRE(elevator);
    look({.23F,1.32F,.34F},{0,1.17F,0});
    const auto b=render("elevator",*elevator);CHECK(a!=b);
    auto processor=earth::add_infrastructure(base,earth::InfrastructureKind::processor,{0,0});REQUIRE(processor);
    look({.19F,1.23F,.23F},{0,1.06F,0});
    const auto processing=render("atmospheric-processor",*processor);CHECK(processing!=b);
    render("atmospheric-processor-diagnostic",*processor,true);
    auto processor_settings=*earth::infrastructure_settings(*processor);
    processor_settings.addon_scale=1.2F;processor_settings.processor_scale=1.3F;
    auto scaled_processor=earth::rebuild_infrastructure(*processor,processor_settings);REQUIRE(scaled_processor);
    CHECK(render("atmospheric-processor-scaled",*scaled_processor)!=processing);
    auto tunnel=earth::add_infrastructure(base,earth::InfrastructureKind::skyway,{-8,0});REQUIRE(tunnel);
    auto recipe=earth::infrastructure_parts(*tunnel)->front();recipe.end={8,0};recipe.terminal_a=recipe.terminal_b=true;
    auto ends=earth::edit_infrastructure(*tunnel,recipe);REQUIRE(ends);
    look({.23F,1.26F,.36F},{0,1.05F,0});render("tunnel-both-ends",*ends);
    auto spaced_recipe=recipe;spaced_recipe.scaffold_spacing=.045F;
    auto spaced=earth::edit_infrastructure(*ends,spaced_recipe);REQUIRE(spaced);
    const auto supported_tunnel=render("tunnel-spaced-supports",*spaced);
    auto manual_recipe=spaced_recipe;
    const auto support_parts=earth::infrastructure_parts(*spaced);REQUIRE(support_parts);
    const auto support_curve=earth::TunnelCurve::create(manual_recipe,*support_parts,*earth::infrastructure_settings(*spaced));REQUIRE(support_curve);
    const auto support_positions=earth::tunnel_support_positions(manual_recipe,*support_curve);REQUIRE(support_positions);REQUIRE(support_positions->size()>1);
    manual_recipe.scaffold_positions=*support_positions;
    (*manual_recipe.scaffold_positions)[1]+=.08F;
    auto manual=earth::edit_infrastructure(*spaced,manual_recipe);REQUIRE(manual);
    const auto support_uploads=runtime->stats().full_mesh_uploads,support_bytes=runtime->stats().vertex_bytes_uploaded;
    CHECK(render("tunnel-manual-support",*manual)!=supported_tunnel);
    CHECK(runtime->stats().full_mesh_uploads==support_uploads);
    CHECK(runtime->stats().vertex_bytes_uploaded-support_bytes<manual->vertex_count*8);
    spaced_recipe.scaffold=false;auto unsupported=earth::edit_infrastructure(*spaced,spaced_recipe);REQUIRE(unsupported);
    CHECK(render("tunnel-no-supports",*unsupported)!=supported_tunnel);
    render("tunnel-restored-ends",*ends);
    auto moved=earth::move_infrastructure_endpoint(*ends,recipe.id,true,{13,8});REQUIRE(moved);
    const auto allocations=runtime->stats().full_mesh_uploads;
    render("tunnel-moved-end",*moved);CHECK(runtime->stats().full_mesh_uploads==allocations);
    render("tunnel-diagnostic",*moved,true);
    const auto* manifest=runtime->diagnostic_manifest();REQUIRE(manifest);REQUIRE(manifest->items().size()==1);
    CHECK(manifest->items()[0].primitives->size()==moved->faces.size());
    // One input, two independently connected output tunnels and a terminal.
    auto connected=earth::add_infrastructure(base,earth::InfrastructureKind::joiner,{0,0});REQUIRE(connected);
    connected=earth::add_infrastructure(*connected,earth::InfrastructureKind::terminal,{7,-12});REQUIRE(connected);
    connected=earth::add_infrastructure(*connected,earth::InfrastructureKind::skyway,{0,12});REQUIRE(connected);
    connected=earth::connect_infrastructure_endpoint(*connected,3,true,{1,1});REQUIRE(connected);
    connected=earth::add_infrastructure(*connected,earth::InfrastructureKind::skyway,{0,-5});REQUIRE(connected);
    connected=earth::connect_infrastructure_endpoint(*connected,4,false,{1,3});REQUIRE(connected);
    connected=earth::connect_infrastructure_endpoint(*connected,4,true,{2,1});REQUIRE(connected);
    connected=earth::add_infrastructure(*connected,earth::InfrastructureKind::skyway,{-10,-12});REQUIRE(connected);
    connected=earth::connect_infrastructure_endpoint(*connected,5,true,{1,2});REQUIRE(connected);
    look({.42F,1.60F,.57F},{0,1.05F,.025F});render("connected-network",*connected);
    look({.16F,1.20F,.23F},{0,1.070F,0});render("joiner-close",*connected);
    look({.33F,1.18F,.36F},{.124F,1.015F,.26F});render("connected-terminal",*connected);
    const auto uploads_before_move=runtime->stats().full_mesh_uploads;
    auto relocated=earth::move_infrastructure(*connected,1,{-2,1});REQUIRE(relocated);
    look({.42F,1.60F,.57F},{0,1.05F,.025F});render("connected-network-moved",*relocated);
    CHECK(runtime->stats().full_mesh_uploads==uploads_before_move);
    render("connected-network-diagnostic",*relocated,true);
    REQUIRE(runtime->diagnostic_manifest());
    CHECK(runtime->diagnostic_manifest()->items()[0].primitives->size()==relocated->faces.size());
    auto free=earth::connect_infrastructure_endpoint(*relocated,4,true,{});REQUIRE(free);
    const auto free_recipe=earth::infrastructure_parts(*free)->at(3);
    const auto direction=earth::placement::direction(free_recipe.end);
    auto elevated=earth::place_infrastructure_endpoint(*free,4,true,earth::placement::mul(direction,1.15F));REQUIRE(elevated);
    look({.42F,1.60F,.57F},{0,1.05F,.025F});
    const auto detached_pixels=render("detached-elevated-end",*elevated);
    const auto free_allocations=runtime->stats().full_mesh_uploads,free_bytes=runtime->stats().vertex_bytes_uploaded;
    auto higher=earth::place_infrastructure_endpoint(*elevated,4,true,earth::placement::mul(direction,1.20F));REQUIRE(higher);
    CHECK(render("detached-higher-end",*higher)!=detached_pixels);
    CHECK(runtime->stats().full_mesh_uploads==free_allocations);
    CHECK(runtime->stats().vertex_bytes_uploaded-free_bytes<higher->vertex_count*8);
    render("detached-elevated-diagnostic",*higher,true);
    REQUIRE(runtime->diagnostic_manifest());
    CHECK(runtime->diagnostic_manifest()->items()[0].primitives->size()==higher->faces.size());
    std::cout<<"Spaceport structure evidence: "<<folder<<'\n';
}

TEST_CASE("Future Earth night layers use the instanced and diagnostic shader paths", "[earth][opengl][infrastructure]") {
    using namespace vng;namespace project=editor_example;namespace earth=example::earth;
    constexpr Extent2D extent{1000,900};
    auto window=test::create_hidden_opengl_window(extent.width,extent.height,"Future Earth acceptance");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current();REQUIRE(token);auto device=opengl::Device::create(*token);REQUIRE(device);
    auto state=earth::author_scene(VNG_EARTH_ASSETS);REQUIRE(state);
    state->document.environment.stars=0;
    auto source=project::mesh_geometry(*state,earth::blueprint_id)->document();
    auto future=earth::rebuild_infrastructure(source,{true,true,true});REQUIRE(future);
    auto future_mesh=editor::EditableMesh::create(*future);
    INFO((future_mesh?"editable future Earth":future_mesh.error().message));REQUIRE(future_mesh);
    *project::mesh_geometry(*state,earth::blueprint_id)=std::move(*future_mesh);
    auto runtime=project::Runtime::create(*device,*state);REQUIRE(runtime);
    gfx::Camera camera;camera.set_position({2.65F,.2F,-1.7F}).look_at({});
    auto pixels=runtime->render(*device,project::RenderRequest{*state,camera,extent,0,false});REQUIRE(pixels);
    CHECK(runtime->stats().last_mesh_draw_calls==1);
    char folder[]="/tmp/vng-future-render-XXXXXX";REQUIRE(::mkdtemp(folder));
    const auto save=[&](std::string_view name,const auto& frame) {
        REQUIRE(example::write_rgba8_png(std::filesystem::path{folder}/(std::string(name)+".png"),extent,
            {reinterpret_cast<const u8*>(frame.pixels.data()),frame.pixels.size()}));
    };
    save("night-all",*pixels);
    // A part gesture reuses the mesh/VAO and uploads only changed vertex ranges.
    const auto allocations=runtime->stats().full_mesh_uploads;
    const auto uploaded=runtime->stats().vertex_bytes_uploaded;
    const auto parts=earth::infrastructure_parts(*future);REQUIRE(parts);
    const auto hub=std::ranges::find(*parts,earth::InfrastructureKind::hub,&earth::InfrastructurePart::kind);REQUIRE(hub!=parts->end());
    auto moved=earth::move_infrastructure(*future,hub->id,{125,8});REQUIRE(moved);
    *project::mesh_geometry(*state,earth::blueprint_id)=*editor::EditableMesh::create(*moved);++state->document.revision;
    REQUIRE(runtime->update_mesh(*device,*state,std::array{earth::blueprint_id}));
    CHECK(runtime->stats().full_mesh_uploads==allocations);
    CHECK(runtime->stats().vertex_bytes_uploaded-uploaded<future->vertex_count*8);
    auto placed=runtime->render(*device,project::RenderRequest{*state,camera,extent,0,false});REQUIRE(placed);
    CHECK(placed->pixels!=pixels->pixels);CHECK(runtime->stats().last_mesh_draw_calls==1);
    save("placed-hub",*placed);
    std::cout<<"Infrastructure move GPU bytes: "<<runtime->stats().vertex_bytes_uploaded-uploaded
             <<" / upload path "<<runtime->stats().last_mesh_update_ms<<" ms\n";
    // Moving an anchor changes the curve, but not its topology or the Earth's
    // other layers. It must remain a scoped vertex update and one draw call.
    const auto tunnel=std::ranges::find_if(*parts,[](const auto& part) {
        return part.kind==earth::InfrastructureKind::skyway&&part.location.x>100&&part.location.x<120;
    });REQUIRE(tunnel!=parts->end());
    const auto before_endpoint=runtime->stats().vertex_bytes_uploaded;
    auto stretched=earth::move_infrastructure_endpoint(*moved,tunnel->id,true,{tunnel->end.x+9,tunnel->end.y-5});REQUIRE(stretched);
    CHECK(stretched->faces==moved->faces);CHECK(stretched->vertex_count==moved->vertex_count);
    *project::mesh_geometry(*state,earth::blueprint_id)=*editor::EditableMesh::create(*stretched);++state->document.revision;
    REQUIRE(runtime->update_mesh(*device,*state,std::array{earth::blueprint_id}));
    CHECK(runtime->stats().full_mesh_uploads==allocations);
    CHECK(runtime->stats().vertex_bytes_uploaded>before_endpoint);
    CHECK(runtime->stats().vertex_bytes_uploaded-before_endpoint<future->vertex_count*8);
    auto endpoint=runtime->render(*device,project::RenderRequest{*state,camera,extent,0,false});REQUIRE(endpoint);
    CHECK(endpoint->pixels!=placed->pixels);CHECK(runtime->stats().last_mesh_draw_calls==1);
    save("moved-tunnel-endpoint",*endpoint);
    std::cout<<"Tunnel endpoint GPU bytes: "<<runtime->stats().vertex_bytes_uploaded-before_endpoint<<'\n';
    std::vector<std::byte> off;
    for(const auto option:std::array{earth::InfrastructureSettings{},earth::InfrastructureSettings{true,false,false},
            earth::InfrastructureSettings{false,true,false},earth::InfrastructureSettings{false,false,true}}) {
        auto changed=earth::rebuild_infrastructure(source,option);REQUIRE(changed);
        auto mesh=editor::EditableMesh::create(*changed);REQUIRE(mesh);
        *project::mesh_geometry(*state,earth::blueprint_id)=std::move(*mesh);++state->document.revision;
        REQUIRE(runtime->update_mesh(*device,*state));
        auto isolated=runtime->render(*device,project::RenderRequest{*state,camera,extent,0,false});REQUIRE(isolated);
        CHECK(isolated->pixels!=pixels->pixels);CHECK(runtime->stats().last_mesh_draw_calls==1);
        if(off.empty())off=isolated->pixels;
        else CHECK(isolated->pixels!=off);
        save(option.night_lights?"cities":option.skyways?"skyways":option.launch_hubs?"hubs":"off",*isolated);
    }
    *project::mesh_geometry(*state,earth::blueprint_id)=*editor::EditableMesh::create(*future);++state->document.revision;
    REQUIRE(runtime->update_mesh(*device,*state));
    state->viewport.selected_object=earth::instance_id;
    auto diagnostic=runtime->render(*device,project::RenderRequest{*state,camera,extent,0,true});REQUIRE(diagnostic);
    const auto* manifest=runtime->diagnostic_manifest();REQUIRE(manifest);REQUIRE(manifest->items().size()==1);
    CHECK(manifest->items()[0].primitives->size()==future->faces.size());
    camera.set_position({-1,2.5F,2}).look_at({});
    auto day=runtime->render(*device,project::RenderRequest{*state,camera,extent,0,false});REQUIRE(day);save("day",*day);
    CHECK(day->pixels!=pixels->pixels);
    // Close inspection in authoring view: graphite shell, raised collars and
    // tiered hubs must read as geometry, not merely differently colored pixels.
    state->viewport.mode=project::ViewMode::mesh;
    camera.set_position({1.22F,.74F,.25F}).look_at({.86F,.40F,.22F});
    auto close=runtime->render(*device,project::RenderRequest{*state,camera,extent,0,false});REQUIRE(close);save("architecture-close",*close);
    CHECK(runtime->stats().last_mesh_draw_calls==1);
    std::cout<<"Future Earth visual evidence: "<<folder<<'\n';
}
TEST_CASE("Earth renders in one instanced batch with matching diagnostic geometry", "[earth][opengl]") {
    using namespace vng;namespace project=editor_example;namespace earth=example::earth;
    auto window=test::create_hidden_opengl_window(320,240,"Earth acceptance");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current();REQUIRE(token);auto device=opengl::Device::create(*token);REQUIRE(device);
    auto state=earth::author_scene(VNG_EARTH_ASSETS);REQUIRE(state);state->viewport.pilot_camera=true;
    auto runtime=project::Runtime::create(*device,*state);INFO((runtime?"ready":runtime.error().message));REQUIRE(runtime);
    constexpr Extent2D extent{320,240};
    auto first=runtime->render(*device,*state,extent,false,0);REQUIRE(first);
    auto second=runtime->render(*device,*state,extent,false,45);REQUIRE(second);
    CHECK_FALSE(std::ranges::equal(first->pixels,second->pixels));
    std::size_t blue{},green{},bright{};
    for(std::size_t i=0;i<first->pixels.size();i+=4) {
        const auto r=std::to_integer<int>(first->pixels[i]),g=std::to_integer<int>(first->pixels[i+1]),b=std::to_integer<int>(first->pixels[i+2]);
        blue+=b>40&&b>r*1.5&&b>g*1.15;green+=g>40&&g>r*1.1&&g>b*1.05;bright+=r>130&&g>130&&b>130;
    }
    CHECK(blue>1000);CHECK(green>100);CHECK(bright>100);
    const auto uploads=runtime->stats().full_mesh_uploads;
    CHECK(runtime->stats().last_mesh_draw_calls==1);CHECK(runtime->stats().last_mesh_instances==1);
    auto another=project::instantiate(*state,earth::blueprint_id);REQUIRE(another);
    project::instance_transform(*state,*another)->position={3,0,0};
    auto batch=runtime->render(*device,*state,extent,false,0);REQUIRE(batch);
    CHECK(runtime->stats().last_mesh_draw_calls==1);CHECK(runtime->stats().last_mesh_instances==2);
    CHECK(runtime->stats().full_mesh_uploads==uploads);
    state->viewport.selected_object=1;
    auto diagnostic=runtime->render(*device,*state,extent,true,0);REQUIRE(diagnostic);
    const auto* manifest=runtime->diagnostic_manifest();REQUIRE(manifest);REQUIRE(manifest->items().size()==1);
    CHECK(manifest->items()[0].provenance.entity==analysis::EntityId{1});
    CHECK(manifest->items()[0].provenance.mesh==analysis::MeshAssetId{3});
    CHECK(manifest->items()[0].primitives->size()==project::mesh_geometry(*state,earth::blueprint_id)->document().faces.size());
    // Metadata edits swap the shader choice; geometry-only drags do not.
    auto changed=project::mesh_geometry(*state,earth::blueprint_id)->document();
    changed.metadata.erase("render/lighting");auto standard=editor::EditableMesh::create(changed);REQUIRE(standard);
    *project::mesh_geometry(*state,earth::blueprint_id)=std::move(*standard);
    REQUIRE(runtime->update_mesh(*device,*state));
    auto smooth=runtime->render(*device,*state,extent,false,0);REQUIRE(smooth);
    CHECK_FALSE(std::ranges::equal(smooth->pixels,batch->pixels));
    changed.metadata["render/lighting"]="illustrated";auto restored=editor::EditableMesh::create(changed);REQUIRE(restored);
    *project::mesh_geometry(*state,earth::blueprint_id)=std::move(*restored);
    REQUIRE(runtime->update_mesh(*device,*state));
    auto again=runtime->render(*device,*state,extent,false,0);REQUIRE(again);CHECK(std::ranges::equal(again->pixels,batch->pixels));
}
TEST_CASE("Cloud edge scatter changes the surface render without runtime particle draws", "[earth][opengl][clouds]") {
    using namespace vng;namespace project=editor_example;namespace earth=example::earth;
    constexpr Extent2D extent{1000,900};
    auto window=test::create_hidden_opengl_window(extent.width,extent.height,"Cloud scatter acceptance");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current();REQUIRE(token);auto device=opengl::Device::create(*token);REQUIRE(device);
    auto state=earth::author_scene(VNG_EARTH_ASSETS);REQUIRE(state);
    state->viewport.mode=project::ViewMode::mesh;
    auto runtime=project::Runtime::create(*device,*state);REQUIRE(runtime);
    const auto original=earth::make_mesh({.edge_scatter=0});
    gfx::Camera camera;camera.set_position({-1.65F,.65F,2.45F}).look_at({0,0,0});
    char folder[]="/tmp/vng-cloud-scatter-XXXXXX";REQUIRE(::mkdtemp(folder));
    std::vector<std::byte> previous;
    for(int amount:{0,1,2}) {
        auto rebuilt=earth::rebuild_clouds(original,{.edge_scatter=static_cast<f32>(amount)});REQUIRE(rebuilt);
        auto mesh=editor::EditableMesh::create(std::move(*rebuilt));REQUIRE(mesh);
        *project::mesh_geometry(*state,earth::blueprint_id)=std::move(*mesh);++state->document.revision;
        REQUIRE(runtime->update_mesh(*device,*state));
        auto pixels=runtime->render(*device,project::RenderRequest{*state,camera,extent,0,false});REQUIRE(pixels);
        CHECK(runtime->stats().last_mesh_draw_calls==1);
        if(!previous.empty())CHECK(previous!=pixels->pixels);
        previous=pixels->pixels;
        REQUIRE(example::write_rgba8_png(std::filesystem::path{folder}/("scatter-"+std::to_string(amount)+".png"),extent,
            {reinterpret_cast<const u8*>(pixels->pixels.data()),pixels->pixels.size()}));
    }
    const auto allocations=runtime->stats().full_mesh_uploads,uploaded_before=runtime->stats().vertex_bytes_uploaded;
    auto moved=earth::move_cloud(project::mesh_geometry(*state,earth::blueprint_id)->document(),15,{-15,5});REQUIRE(moved);
    auto relocated=editor::EditableMesh::create(std::move(*moved));REQUIRE(relocated);
    *project::mesh_geometry(*state,earth::blueprint_id)=std::move(*relocated);++state->document.revision;
    REQUIRE(runtime->update_mesh(*device,*state,std::array{earth::blueprint_id}));
    CHECK(runtime->stats().full_mesh_uploads==allocations);
    CHECK(runtime->stats().vertex_bytes_uploaded-uploaded_before<project::mesh_geometry(*state,earth::blueprint_id)->size()*8);
    auto pixels=runtime->render(*device,project::RenderRequest{*state,camera,extent,0,false});REQUIRE(pixels);
    CHECK(previous!=pixels->pixels);CHECK(runtime->stats().last_mesh_draw_calls==1);
    std::cout<<"Cloud move GPU bytes: "<<runtime->stats().vertex_bytes_uploaded-uploaded_before
        <<" / CPU upload path "<<runtime->stats().last_mesh_update_ms<<" ms\n";
    REQUIRE(example::write_rgba8_png(std::filesystem::path{folder}/"moved-spiral.png",extent,
        {reinterpret_cast<const u8*>(pixels->pixels.data()),pixels->pixels.size()}));
    std::cout<<"Cloud scatter and placement visual evidence: "<<folder<<'\n';
}
