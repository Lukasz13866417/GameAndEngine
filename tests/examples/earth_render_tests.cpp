#include "scenes/earth_scene.hpp"
#include "support/earth_assets.hpp"
#include "support/presentation.hpp"
#include "editor/runtime.hpp"
#include "editor/animation.hpp"
#include "editor/mesh_shading.hpp"
#include "../support/glfw_opengl.hpp"
#include <vng/analysis/manifest.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <iostream>
#include <algorithm>

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
