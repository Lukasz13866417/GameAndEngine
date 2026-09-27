#include "scenes/tunnel_scene.hpp"
#include "scenes/tunnel_departure.hpp"
#include "support/presentation.hpp"
#include "editor/animation.hpp"
#include "editor/runtime.hpp"
#include "../support/glfw_opengl.hpp"
#include <vng/analysis/manifest.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <iostream>

TEST_CASE("Tunnel haze, bloom and diagnostics share the batched scene renderer", "[tunnel][opengl]") {
    using namespace vng;
    namespace project=editor_example;
    constexpr Extent2D extent{640,400};
    auto window=test::create_hidden_opengl_window(extent.width,extent.height,"Tunnel render test");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current();REQUIRE(token);
    auto device=opengl::Device::create(*token);REQUIRE(device);
    auto scene=example::tunnel::author_scene(VNG_TUNNEL_ASSETS);REQUIRE(scene);
    auto runtime=project::Runtime::create(*device,*scene);REQUIRE(runtime);
    const auto camera=project::camera(project::evaluate_camera(*scene,9),project::ViewMode::scene);
    auto rendered=runtime->render(*device,{*scene,camera,extent,9,false});REQUIRE(rendered);
    CHECK(runtime->stats().last_mesh_draw_calls==3);
    CHECK(runtime->stats().last_mesh_renderer_calls==3);
    const auto uploads=runtime->stats().full_mesh_uploads;
    // Near-white deep light, distinct from cooler peripheral structure. Keep bounds
    // loose enough to tolerate driver differences in bloom filtering.
    const auto pixel=[&](u32 x,u32 y,u32 c){return std::to_integer<unsigned>(rendered->pixels[(y*extent.width+x)*4+c]);};
    CHECK(std::abs(static_cast<int>(pixel(320,200,0))-static_cast<int>(pixel(320,200,2)))<20);
    CHECK(pixel(320,200,2)>200U);
    scene->document.environment.bloom_strength=0;
    ++scene->document.revision;
    auto plain=runtime->render(*device,{*scene,camera,extent,9,false});REQUIRE(plain);
    CHECK(plain->pixels!=rendered->pixels);
    CHECK(runtime->stats().full_mesh_uploads==uploads);
    const auto end_camera=project::camera(project::evaluate_camera(*scene,36),project::ViewMode::scene);
    auto end=runtime->render(*device,{*scene,end_camera,extent,36,false});REQUIRE(end);
    CHECK(end->pixels!=plain->pixels);
    CHECK(runtime->stats().full_mesh_uploads==uploads);
    // The same resources also draw an oblique view through the open mouth:
    // exterior and interior coexist without reclassifying/reuploading a ticket.
    gfx::Camera outside_camera;
    outside_camera.set_position({5,3,20}).look_at({0,0,-5});
    const auto outside=runtime->render(*device,{*scene,outside_camera,extent,9,false});REQUIRE(outside);
    CHECK(outside->pixels!=plain->pixels);
    CHECK(runtime->stats().full_mesh_uploads==uploads);
    CHECK(runtime->stats().last_mesh_draw_calls==3);
    char folder[]="/tmp/vng-tunnel-sides-XXXXXX";REQUIRE(::mkdtemp(folder));
    REQUIRE(example::write_rgba8_png(std::filesystem::path{folder}/"outside.png",extent,
        {reinterpret_cast<const u8*>(outside->pixels.data()),outside->pixels.size()}));
    REQUIRE(example::write_rgba8_png(std::filesystem::path{folder}/"inside.png",extent,
        {reinterpret_cast<const u8*>(plain->pixels.data()),plain->pixels.size()}));
    std::cout<<"Tunnel inside/outside evidence: "<<folder<<'\n';
    scene->viewport.selected_object=1;
    auto diagnostic=runtime->render(*device,{*scene,camera,extent,9,true});REQUIRE(diagnostic);
    const auto* manifest=runtime->diagnostic_manifest();REQUIRE(manifest);
    REQUIRE(manifest->items().size()==1);
    CHECK(manifest->items()[0].primitives->size()==scene->document.mesh.document().faces.size());
}

TEST_CASE("Departure renders traffic, the open terminal and Earth without exterior tunnel fog", "[tunnel][departure][opengl]") {
    using namespace vng;namespace p=editor_example;namespace d=example::tunnel::departure;
    constexpr Extent2D extent{640,400};
    auto window=test::create_hidden_opengl_window(extent.width,extent.height,"Departure render test");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current();REQUIRE(token);auto device=opengl::Device::create(*token);REQUIRE(device);
    auto state=d::author_scene(VNG_TUNNEL_ASSETS);REQUIRE(state);
    auto runtime=p::Runtime::create(*device,*state);REQUIRE(runtime);
    auto asset=std::ranges::find(state->document.mesh_assets,static_cast<p::BlueprintId>(5),&p::MeshBlueprint::id);
    REQUIRE(asset!=state->document.mesh_assets.end());
    const auto full_planet=asset->geometry;
    auto isolated=full_planet.document();
    const auto catalogue=example::earth::infrastructure_parts(isolated);REQUIRE(catalogue);
    const auto route=std::ranges::find(*catalogue,example::earth::express_route::name,&example::earth::InfrastructurePart::name);REQUIRE(route!=catalogue->end());
    const auto ids=std::ranges::find(isolated.vertex_fields,example::earth::infrastructure_part_field,&content::vmesh::VertexField::name);REQUIRE(ids!=isolated.vertex_fields.end());
    const auto& owners=std::get<std::vector<u32>>(ids->values);
    std::erase_if(isolated.faces,[&](const auto& face){return owners[face.vertices[0]]!=route->id;});
    isolated.edges.reset();
    const auto only_route=editor::EditableMesh::create(std::move(isolated));REQUIRE(only_route);
    // Regression: Earth/addons render after the stars. They must not leak
    // through the white opening, regardless of their own material's fog style.
    // Removing the distant scenery should be visually indistinguishable early,
    // but must make a visible difference once we are at the real exit.
    for(const auto time:{0.F,10.F,d::exit_time+.5F}) {
        INFO("Veil comparison at "<<time);
        const auto camera=p::render_camera(*state,time);
        auto shown=runtime->render(*device,{*state,camera,extent,time,false});REQUIRE(shown);
        asset->geometry=*only_route;
        ++state->document.revision;
        REQUIRE(runtime->update_mesh(*device,*state));
        auto hidden=runtime->render(*device,{*state,camera,extent,time,false});REQUIRE(hidden);
        unsigned changed{},maximum{};
        for(std::size_t i=0;i<shown->pixels.size();++i) {
            const auto difference=static_cast<unsigned>(std::abs(std::to_integer<int>(shown->pixels[i])-std::to_integer<int>(hidden->pixels[i])));
            maximum=std::max(maximum,difference);changed+=difference>2;
        }
        if(time<11){CHECK(maximum<=2U);CHECK(changed==0U);}
        else CHECK(changed>1000U);
        asset->geometry=full_planet;
        ++state->document.revision;
        REQUIRE(runtime->update_mesh(*device,*state));
    }
    // Look through the real, open portal: empty sky must receive haze too.
    // Otherwise a black disk remains visible even when the walls are white.
    unsigned previous_light=256;
    for(const auto distance:{120.F,40.F,15.F,2.F}) {
        gfx::Camera probe;probe.set_position(example::tunnel::exit_center(distance)).look_at({0,0,0});
        auto image=runtime->render(*device,{*state,probe,extent,0,false});REQUIRE(image);
        const auto offset=(200*extent.width+320)*4;
        const auto light=std::to_integer<unsigned>(image->pixels[offset+2]);
        INFO("Exit distance "<<distance<<" / blue "<<light);
        CHECK(light<=previous_light+2);previous_light=light;
        if(distance==120)CHECK(light>215U);
        if(distance==2)CHECK(light<180U);
    }
    // Through the skyway, then one frame in each of the voyage's locations:
    // Earth and the gateway, the Moon base under Earth, and the fleet.
    std::vector<std::byte> previous;
    for(const auto time:{1.F,3.F,d::courier_arrival+.15F,d::catch_up_begin+.35F,d::catch_up_end+.1F,18.F,d::throat_time-1,d::exit_time+.5F,32.F,57.8F,100.F}) {
        INFO("Departure time "<<time);
        const auto camera=p::render_camera(*state,time);
        auto pixels=runtime->render(*device,{*state,camera,extent,time,false});REQUIRE(pixels);
        CHECK(pixels->pixels!=previous);previous=pixels->pixels;
        CHECK(runtime->stats().last_mesh_draw_calls<=state->document.mesh_assets.size());
        if(time==32) {
            unsigned dark{},colored{};
            for(std::size_t i=0;i<pixels->pixels.size();i+=4) {
                const auto r=std::to_integer<unsigned>(pixels->pixels[i]);
                const auto g=std::to_integer<unsigned>(pixels->pixels[i+1]);
                const auto b=std::to_integer<unsigned>(pixels->pixels[i+2]);
                dark+=r<30&&g<30&&b<40;colored+=b>r+25;
            }
            CHECK(dark>extent.width*extent.height/10);
            CHECK(colored>extent.width*extent.height/50);
        }
    }
}
