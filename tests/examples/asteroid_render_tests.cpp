#include "editor/runtime.hpp"
#include "editor/animation.hpp"
#include "support/asteroid_scene.hpp"
#include "../support/glfw_opengl.hpp"
#include <vng/analysis/manifest.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <iostream>

TEST_CASE("Asteroid belt occludes the fleet and reveals it using the editable camera", "[asteroid][opengl]") {
    using namespace vng;
    namespace project=editor_example;
    constexpr Extent2D extent{480,300};
    auto window=test::create_hidden_opengl_window(extent.width,extent.height,"asteroid fleet acceptance");
    if(!window) { std::cerr<<window.error().message<<'\n';std::exit(77); }
    auto current=window->make_current(); REQUIRE(current);
    auto device=opengl::Device::create(*current); REQUIRE(device);
    // Author the shot from its generator; the shipped .vscene is a user's
    // editable project and may no longer match the authored fixture.
    auto state=example::asteroids::author_scene(VNG_ASTEROID_ASSETS); REQUIRE(state);
    state->viewport.pilot_camera=true;
    auto runtime=project::Runtime::create(*device,*state);
    INFO((runtime?"ready":runtime.error().message)); REQUIRE(runtime);
    auto no_fleet=*state;
    for(auto& instance:no_fleet.document.instances)
        if(instance.id!=example::asteroids::hero && !example::asteroids::is_rock(instance))
            std::get<project::MeshSettings>(instance.settings).visible=false;
    auto clear_fleet=*state,clear_empty=no_fleet;
    for(auto* variant:{&clear_fleet,&clear_empty})
        for(auto& instance:variant->document.instances)
            if(example::asteroids::is_rock(instance))
                std::get<project::MeshSettings>(instance.settings).visible=false;
    const auto changed=[](const gfx::ImageData& a,const gfx::ImageData& b) {
        std::size_t count{};
        for(std::size_t i=0;i<a.pixels.size();i+=4) {
            bool different{};
            for(std::size_t c=0;c<3;++c)
                different|=std::abs(std::to_integer<int>(a.pixels[i+c])-std::to_integer<int>(b.pixels[i+c]))>1;
            count+=different;
        }
        return count;
    };
    for(const auto time:{0.F,10.F,18.F,26.F,38.F,52.F}) {
        auto full=runtime->render(*device,*state,extent,false,time); REQUIRE(full);
        auto empty=runtime->render(*device,no_fleet,extent,false,time); REQUIRE(empty);
        const auto pixels=changed(*full,*empty);
        auto unobstructed=runtime->render(*device,clear_fleet,extent,false,time); REQUIRE(unobstructed);
        auto background=runtime->render(*device,clear_empty,extent,false,time); REQUIRE(background);
        const auto possible=changed(*unobstructed,*background);
        INFO("time "<<time<<": "<<pixels<<" visible / "<<possible<<" unobstructed fleet pixels");
        std::cout<<"Belt at "<<time<<"s: "<<pixels<<" / "<<possible<<" fleet pixels\n";
        REQUIRE(possible>500);
        // Depth removes some visibility, not all of it. Do not regress to a
        // solid wall or manufacture the reveal using a far-plane/visibility cut.
        if(time<=10.F) { CHECK(pixels>0); CHECK(pixels<possible*.8); }
        if(time>=38.F) { CHECK(pixels>500); CHECK(pixels>possible*.95); }
    }
    CHECK(runtime->stats().resident_meshes==7);
    CHECK(runtime->stats().full_mesh_uploads==7);
    CHECK(runtime->stats().vertex_updates==0);
    state->viewport.selected_object=19;
    auto diagnostic=runtime->render(*device,*state,extent,true,0.F); REQUIRE(diagnostic);
    const auto* manifest=runtime->diagnostic_manifest(); REQUIRE(manifest);
    REQUIRE(manifest->items().size()==1);
    CHECK(manifest->items()[0].provenance.entity==analysis::EntityId{19});
    CHECK(manifest->items()[0].provenance.mesh==analysis::MeshAssetId{6});
    CHECK(manifest->items()[0].primitives->size()==5120);
}
