#include "../../examples/editor/runtime.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/keyframes.hpp"
#include "../../examples/editor/edits.hpp"
#include "../../examples/editor/effects.hpp"
#include "../../examples/editor/selection.hpp"
#include "../support/glfw_opengl.hpp"
#include <catch2/catch_test_macros.hpp>
#include <vng/opengl/gpu_mesh.hpp>
#include <vng/analysis/manifest.hpp>
#include <vng/opengl/render_target.hpp>
#include <vng/render/frame.hpp>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>
#include <glad/gl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using namespace vng;
using Clock = std::chrono::steady_clock;
editor_example::State make_state() {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position",
                               {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>{-.8F, -.7F, 0, .8F, -.7F, 0, 0, .8F, 0}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    editor_example::State state{.document = {.mesh = std::move(*mesh)}};
    state.viewport.mode = editor_example::ViewMode::mesh;
    state.viewport.selected_object = 1;
    state.viewport.editor_camera.yaw = 0;
    state.viewport.editor_camera.pitch = 0;
    return state;
}
struct ImportDirectory {
    std::filesystem::path path;
    ImportDirectory() {
        char name[] = "/tmp/vng-runtime-import-XXXXXX";
        const auto* created = ::mkdtemp(name);
        REQUIRE(created);
        path = created;
    }
    ~ImportDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
} // namespace
TEST_CASE("Region grids and world bounds depth-test against scene geometry without affecting play", "[editor][opengl][region][depth]") {
    using namespace editor_example;
    auto window=test::create_hidden_opengl_window(128,128,"annotation depth");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current();REQUIRE(token);auto device=opengl::Device::create(*token);REQUIRE(device);
    auto state=make_state();state.viewport.mode=ViewMode::scene;
    state.viewport.editor_camera.distance=3;
    mesh_settings(state,1)->visible=true;
    sun_settings(state,2)->visible=false;
    instance_transform(state,1)->scale=1;
    instance_transform(state,1)->position={};
    auto id=instantiate(state,BlueprintId::region);REQUIRE(id);
    auto* region=region_settings(state,*id);
    region->boundary={.points={{-.15F,0,-.4F},{.15F,0,-.4F}},.loose_edges={{0,1}}};
    state.document.world_bounds={{-.12F,-.1F,-.5F},{.12F,.1F,-.3F}};
    auto runtime=Runtime::create(*device,state);REQUIRE(runtime);
    auto camera_=camera(state.viewport.editor_camera,ViewMode::scene);
    const auto capture=[&](bool annotations) {
        ++state.document.revision;
        auto image=runtime->render(*device,RenderRequest{state,camera_,{128,128},0,false,annotations});
        if(!image)INFO(image.error().message);
        REQUIRE(image);return *image;
    };
    const auto plain=capture(false);
    state.viewport.show_regions=true;state.viewport.show_world_bounds=true;
    CHECK(std::ranges::equal(capture(true).pixels,plain.pixels)); // Both outlines lie wholly behind the mesh.
    for(auto& point:region_settings(state,*id)->boundary.points)point.z=.4F;
    CHECK(capture(true).pixels!=plain.pixels);
    CHECK(capture(false).pixels==plain.pixels); // Independent play ignores preview decorations.
    state.viewport.show_world_bounds=false;
    region_settings(state,*id)->boundary=make_region(RegionShape::box,{0,0,.6F},.2F);
    const auto edges=capture(true);
    region_settings(state,*id)->show_walls=true;
    CHECK(capture(true).pixels!=edges.pixels);
    // Borrowed depth/color attachments must be rebound after a resize.
    auto resized=runtime->render(*device,RenderRequest{state,camera_,{160,100},0,false,true});REQUIRE(resized);
    CHECK(resized->extent==Extent2D{160,100});
}
TEST_CASE("Gizmo-only preview hides the active surface without changing assets or production rendering", "[editor][opengl][gizmo-only]") {
    using namespace editor_example;
    auto window=test::create_hidden_opengl_window(128,128,"gizmo-only preview");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current();REQUIRE(token);auto device=opengl::Device::create(*token);REQUIRE(device);
    auto state=make_state();state.viewport.mode=ViewMode::scene;
    state.viewport.editor_camera.distance=3;
    sun_settings(state,2)->visible=false;
    instance_transform(state,1)->scale=1;instance_transform(state,1)->position={};
    auto runtime=Runtime::create(*device,state);REQUIRE(runtime);
    const auto original=encode_scene(state);REQUIRE(original);
    const auto capture=[&](bool editor,bool diagnostic=false) {
        auto image=runtime->render(*device,RenderRequest{state,camera(state.viewport.editor_camera,ViewMode::scene),{128,128},0,diagnostic,editor});
        REQUIRE(image);return image->pixels;
    };
    const auto normal=capture(true);
    state.viewport.gizmo_only=true;
    const auto hidden=capture(true);CHECK(hidden!=normal);
    CHECK(runtime->stats().last_mesh_instances==0);
    CHECK(capture(true,true)==hidden); // Diagnostic rendering cannot resurrect the hidden surface.
    CHECK(capture(false)==normal); // Independent Play ignores the private toggle.
    const auto after=encode_scene(state);REQUIRE(after);CHECK(*original==*after);
    state.viewport.selected_object=0;
    CHECK(capture(true)==normal);
    state.viewport.selected_object=2;
    sun_settings(state,2)->visible=true;++state.document.revision;
    state.viewport.gizmo_only=false;
    const auto with_sun=capture(true);
    state.viewport.gizmo_only=true;
    CHECK(capture(true)!=with_sun);
    CHECK(capture(false)==with_sun);
}
TEST_CASE("Live preview asynchronous transfer preserves pixels across resize and final idle frames", "[editor][opengl][async]") {
    auto window=test::create_hidden_opengl_window(64,64,"runtime asynchronous preview");
    if(!window) {std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current(); REQUIRE(token);
    auto device=opengl::Device::create(*token); REQUIRE(device);
    auto state=make_state();
    auto runtime=editor_example::Runtime::create(*device,state); REQUIRE(runtime);
    u64 id{};
    for(const auto extent:{Extent2D{64,48},Extent2D{128,96},Extent2D{32,24}}) {
        REQUIRE(runtime->render_frame(*device,state,{128,96},0.F));
        auto expected=runtime->readback(*device,extent); REQUIRE(expected);
        auto queued=runtime->queue_readback(*device,extent,++id); REQUIRE(queued); REQUIRE(*queued);
        CHECK(runtime->readback_pending());
        std::optional<opengl::Rgba8Readback> completed;
        const auto deadline=Clock::now()+std::chrono::seconds(3);
        // No subsequent render, input, or glFinish needed to complete the final edit.
        while(!completed && Clock::now()<deadline) {
            auto result=runtime->poll_readback(); REQUIRE(result);
            completed=std::move(*result);
            if(!completed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(completed);
        CHECK(completed->id==id); CHECK(completed->image.extent==extent);
        CHECK(completed->image.pixels==expected->pixels);
        CHECK_FALSE(runtime->readback_pending());
    }
    CHECK(runtime->stats().full_mesh_uploads==1);
    auto cancelled=runtime->queue_readback(*device,{32,24},++id); REQUIRE(cancelled); REQUIRE(*cancelled);
    runtime->discard_readbacks();
    const auto deadline=Clock::now()+std::chrono::seconds(3);
    while(runtime->readback_pending() && Clock::now()<deadline) {
        auto result=runtime->poll_readback(); REQUIRE(result); CHECK_FALSE(*result);
        if(runtime->readback_pending()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_FALSE(runtime->readback_pending()); CHECK(runtime->readback_available());
}

TEST_CASE("Mesh hiding changes only inspection indices and preserves diagnostic face IDs", "[editor][opengl][hide]") {
    using namespace editor_example;
    constexpr Extent2D extent{96,96};
    auto state=make_state();auto document=state.document.mesh.document();
    document.vertex_count=6;
    std::get<std::vector<f32>>(document.vertex_fields[0].values).insert(
        std::get<std::vector<f32>>(document.vertex_fields[0].values).end(),
        {-.8F,-.7F,-.2F,.8F,-.7F,-.2F,0,.8F,-.2F});
    document.faces.emplace_back(3,4,5);
    document.vertex_fields.push_back({"color/0",{content::vmesh::ScalarType::Float32,3},
        std::vector<f32>{1,0,0,1,0,0,1,0,0,0,1,0,0,1,0,0,1,0}});
    auto mesh=editor::EditableMesh::create(document);REQUIRE(mesh);state.document.mesh=std::move(*mesh);
    sun_settings(state,2)->visible=false;
    *instance_transform(state,1)={};
    const auto revision=state.document.revision;
    auto window=test::create_hidden_opengl_window(96,96,"mesh visibility");
    if(!window) {std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto token=window->make_current();REQUIRE(token);auto device=opengl::Device::create(*token);REQUIRE(device);
    auto runtime=Runtime::create(*device,state);REQUIRE(runtime);
    gfx::Camera camera;camera.set_position({0,0,5}).look_at({0,0,0});
    const auto image=[&](bool diagnostic=false) {
        auto rendered=runtime->render(*device,RenderRequest{state,camera,extent,0,diagnostic});
        if(!rendered) INFO(rendered.error().message);
        REQUIRE(rendered);return std::move(*rendered);
    };
    const auto center=[](const gfx::ImageData& im,unsigned channel) {
        return std::to_integer<unsigned>(im.pixels[(48*96+48)*4+channel]);
    };
    const auto original=image();CHECK(center(original,0)>100);CHECK(center(original,1)<20);
    const auto uploads=runtime->stats().full_mesh_uploads;
    MeshVisibility visibility{BlueprintId::mesh,gfx::detail::mesh_topology_fingerprint(6,document.faces),{0}};
    runtime->mesh_visibility(visibility);
    const auto hidden_front=image();CHECK(center(hidden_front,1)>100);CHECK(center(hidden_front,0)<20);
    const auto diagnostic=image(true);
    CHECK(center(diagnostic,0)==244);CHECK(center(diagnostic,1)==146);CHECK(center(diagnostic,2)==138); // original face 1, not renumbered to 0
    visibility.hidden_faces={0,1};runtime->mesh_visibility(visibility);
    const auto hidden_all=image();CHECK(center(hidden_all,0)<40);CHECK(center(hidden_all,1)<40);
    state.viewport.mode=ViewMode::scene;
    const auto scene=image();CHECK(center(scene,0)>100);CHECK(center(scene,1)<20);
    state.viewport.mode=ViewMode::mesh;
    CHECK(image().pixels==hidden_all.pixels); // scene drawing did not consume the mask
    visibility.hidden_faces.clear();runtime->mesh_visibility(visibility);
    CHECK(image().pixels==original.pixels);
    visibility.hidden_faces={0,1};++visibility.topology.low;runtime->mesh_visibility(visibility);
    CHECK(image().pixels==original.pixels); // stale topology cannot hide unrelated faces
    CHECK(runtime->stats().full_mesh_uploads==uploads);CHECK(runtime->stats().vertex_updates==0);
    CHECK(state.document.revision==revision);CHECK(state.document.mesh.document()==document);
    // Render-only authoring matrices affect normal and diagnostic passes without
    // uploads/rebuilds, and remain private until Apply.
    auto placement=Mat4::identity();placement[0][0]=placement[1][1]=.25F;
    state.document.mesh_placements[BlueprintId::mesh].draft=placement;
    const auto scaled=image();CHECK(scaled.pixels!=original.pixels);
    CHECK(image(true).pixels!=diagnostic.pixels);
    CHECK(runtime->stats().full_mesh_uploads==uploads);CHECK(runtime->stats().vertex_updates==0);
    state.viewport.mode=ViewMode::scene;CHECK(image().pixels==original.pixels);
    REQUIRE(apply_mesh_draft(state,BlueprintId::mesh));
    CHECK(image().pixels==scaled.pixels);
}
TEST_CASE("Imported mesh blueprints render distinct geometry without replacing or duplicating assets",
          "[editor][opengl][import]") {
    using namespace editor_example;
    constexpr Extent2D extent{320, 240};
    ImportDirectory files;
    auto triangle = make_state().document.mesh.document();
    triangle.metadata["name"] = "Imported green triangle";
    triangle.vertex_fields.push_back({"color/0", {content::vmesh::ScalarType::Float32, 3},
                                      std::vector<f32>{.02F, 1, .04F, .02F, 1, .04F, .02F, 1, .04F}});
    const auto triangle_path = files.path / "triangle.vmesh";
    REQUIRE(content::vmesh::write_vmesh(triangle_path, triangle));
    const auto cube_path = std::filesystem::path(__FILE__).parent_path() /
                           "../../examples/assets/colored_cube.vmesh";
    auto cube = editor::EditableMesh::load(cube_path);
    REQUIRE(cube);
    REQUIRE(cube->document().faces.size() == 12);
    State state{.document = {.mesh = std::move(*cube)}};
    state.viewport.selected_object = 1;
    state.viewport.editor_camera = {.yaw = 0, .pitch = 0, .distance = 6};
    instance_transform(state, 1)->position = {-1.2F, 0, 0};
    sun_settings(state, 2)->visible = false;
    const auto original_cube = state.document.mesh.document();
    auto window = test::create_hidden_opengl_window(extent.width, extent.height, "mesh blueprint import");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto runtime = Runtime::create(*device, state);
    REQUIRE(runtime);
    auto original_image = runtime->render(*device, state, extent, false);
    REQUIRE(original_image);
    const auto imported = import_mesh(state, triangle_path);
    REQUIRE(imported);
    const auto blueprint = find_instance(state, *imported)->blueprint;
    REQUIRE(blueprint != BlueprintId::mesh);
    instance_transform(state, *imported)->position = {1.2F, 0, 0};
    instance_transform(state, *imported)->scale = .7F;
    REQUIRE(runtime->update_mesh(*device, state));
    CHECK(runtime->stats().resident_meshes == 2);
    CHECK(runtime->stats().full_mesh_uploads == 2);
    CHECK(runtime->stats().vertex_updates == 0);
    CHECK(state.document.mesh.document() == original_cube);
    CHECK(mesh_geometry(state, blueprint)->document().vertex_fields[1].type ==
          content::vmesh::FieldType{content::vmesh::ScalarType::Float32, 3});
    auto both = runtime->render(*device, state, extent, false);
    REQUIRE(both);
    std::size_t blue_left{}, green_right{}, unchanged_left{};
    const auto same_left = [&](const gfx::ImageData& a, const gfx::ImageData& b) {
        for (u32 y = 0; y < extent.height; ++y) {
            const auto row = static_cast<std::size_t>(y) * extent.width * 4;
            if (!std::equal(a.pixels.begin() + static_cast<std::ptrdiff_t>(row),
                            a.pixels.begin() + static_cast<std::ptrdiff_t>(row + extent.width * 2),
                            b.pixels.begin() + static_cast<std::ptrdiff_t>(row))) return false;
        }
        return true;
    };
    for (u32 y = 0; y < extent.height; ++y)
        for (u32 x = 0; x < extent.width; ++x) {
            const auto at = (static_cast<std::size_t>(y) * extent.width + x) * 4;
            const auto red = std::to_integer<int>(both->pixels[at]);
            const auto green = std::to_integer<int>(both->pixels[at + 1]);
            const auto blue = std::to_integer<int>(both->pixels[at + 2]);
            if (x < extent.width / 2 && blue > 100 && blue > green + 20) ++blue_left;
            if (x >= extent.width / 2 && green > 100 && green > red + 40 && green > blue + 40) ++green_right;
            if (x < extent.width / 2 && both->pixels[at] == original_image->pixels[at]) ++unchanged_left;
        }
    CHECK(blue_left > 100);
    CHECK(green_right > 100);
    CHECK(unchanged_left == extent.width / 2 * extent.height);
    CHECK(same_left(*both, *original_image));
    // CPU picking must use the imported triangle, not the cube's topology.
    bool picked_imported = false;
    for (u32 y = 70; y < 170 && !picked_imported; y += 5)
        for (u32 x = 170; x < 280 && !picked_imported; x += 5)
            picked_imported = pick_object(state, {static_cast<f32>(x) / extent.width,
                                                  static_cast<f32>(y) / extent.height}, extent) == *imported;
    CHECK(picked_imported);
    state.viewport.selected_object = *imported;
    auto diagnostic = runtime->render(*device, state, extent, true);
    REQUIRE(diagnostic);
    const auto* manifest = runtime->diagnostic_manifest();
    REQUIRE(manifest);
    REQUIRE(manifest->items().size() == 1);
    CHECK(manifest->items()[0].provenance.entity == analysis::EntityId{*imported});
    CHECK(manifest->items()[0].provenance.mesh == analysis::MeshAssetId{static_cast<u32>(blueprint)});
    CHECK(manifest->items()[0].primitives->size() == 1);
    CHECK(manifest->items()[0].primitives->sources()[0].vertices == gfx::TriangleFace{0, 1, 2});
    state.viewport.selected_object = 1;
    REQUIRE(runtime->render(*device, state, extent, true));
    manifest = runtime->diagnostic_manifest();
    REQUIRE(manifest);
    CHECK(manifest->items()[0].provenance.entity == analysis::EntityId{1});
    CHECK(manifest->items()[0].provenance.mesh == analysis::MeshAssetId{1});
    CHECK(manifest->items()[0].primitives->size() == 12);

    state.viewport.selected_object = *imported;
    auto* imported_mesh = mesh_geometry(state, blueprint);
    REQUIRE(imported_mesh);
    VertexEdit edit{state.document.revision, state.document.revision + 1, {{2, {0, .3F, 0}}}, static_cast<u32>(blueprint)};
    const auto encoded = encode_edit(edit);
    REQUIRE(encoded);
    const auto decoded = decode_edit(*encoded);
    REQUIRE(decoded);
    REQUIRE(apply_edit(state, *decoded));
    REQUIRE(runtime->update_mesh(*device, state));
    auto edited_image = runtime->render(*device, state, extent, false);
    REQUIRE(edited_image);
    CHECK(edited_image->pixels != both->pixels);
    CHECK(same_left(*edited_image, *both));
    CHECK(state.document.mesh.document() == original_cube);
    CHECK(runtime->stats().full_mesh_uploads == 2);
    CHECK(runtime->stats().vertex_updates == 1);
    CHECK(runtime->stats().vertex_bytes_uploaded == 16);
    CHECK(runtime->diagnostic_manifest() == nullptr);

    // A later invalid blueprint must not leave an earlier asset's buffered
    // position edit applied when rejecting the multi-asset update.
    auto rejected = state;
    rejected.document.mesh_assets.push_back(rejected.document.mesh_assets.front());
    REQUIRE(rejected.document.mesh.set_position(0, {0, 0, 0}));
    CHECK_FALSE(runtime->update_mesh(*device, rejected));
    auto retained = runtime->render(*device, state, extent, false);
    REQUIRE(retained);
    CHECK(retained->pixels == edited_image->pixels);
    CHECK(runtime->stats().resident_meshes == 2);
    CHECK(runtime->stats().vertex_updates == 1);

    // Other optional color layouts remain authored exactly as imported; only
    // the preview falls back to white and reports that choice.
    auto unusual = state;
    auto unusual_document = mesh_geometry(unusual, blueprint)->document();
    unusual_document.vertex_fields[1].type = {content::vmesh::ScalarType::UInt32, 2};
    unusual_document.vertex_fields[1].values = std::vector<u32>(6, 17);
    auto unusual_mesh = editor::EditableMesh::create(std::move(unusual_document));
    REQUIRE(unusual_mesh);
    *mesh_geometry(unusual, blueprint) = std::move(*unusual_mesh);
    REQUIRE(runtime->update_mesh(*device, unusual));
    auto white = runtime->render(*device, unusual, extent, false);
    REQUIRE(white);
    CHECK(white->pixels != retained->pixels);
    CHECK(same_left(*white, *retained));
    CHECK(runtime->diagnostics().find("optional color/0 ignored") != std::string_view::npos);
    CHECK(mesh_geometry(unusual, blueprint)->document().vertex_fields[1].type ==
          content::vmesh::FieldType{content::vmesh::ScalarType::UInt32, 2});
    REQUIRE(runtime->update_mesh(*device, state));

    const auto sibling = instantiate(state, blueprint);
    REQUIRE(sibling);
    instance_transform(state, *sibling)->position = {1.2F, -.8F, 0};
    instance_transform(state, *sibling)->scale = .35F;
    REQUIRE(runtime->update_mesh(*device, state));
    auto shared = runtime->render(*device, state, extent, false);
    REQUIRE(shared);
    CHECK(shared->pixels != retained->pixels);
    CHECK(runtime->stats().full_mesh_uploads == 2);
    CHECK(runtime->stats().resident_meshes == 2);
    REQUIRE(erase_instance(state, *imported));
    REQUIRE(erase_instance(state, *sibling));
    REQUIRE(runtime->update_mesh(*device, state));
    auto only_cube = runtime->render(*device, state, extent, false);
    REQUIRE(only_cube);
    CHECK(only_cube->pixels == original_image->pixels);
    CHECK(state.document.mesh_assets.size() == 1);
    CHECK(runtime->stats().resident_meshes == 2);
    REQUIRE(instantiate(state, blueprint));
    REQUIRE(runtime->update_mesh(*device, state));
    REQUIRE(runtime->render(*device, state, extent, false));
    CHECK(runtime->stats().full_mesh_uploads == 2);
    auto serialized = encode(state);
    REQUIRE(serialized);
    auto reloaded = decode(*serialized);
    REQUIRE(reloaded);
    REQUIRE(runtime->update_mesh(*device, *reloaded));
    auto reopened = runtime->render(*device, *reloaded, extent, false);
    REQUIRE(reopened);
    CHECK(runtime->stats().full_mesh_uploads == 2);
    CHECK(reloaded->document.mesh.document() == original_cube);
    CHECK(glGetError() == GL_NO_ERROR);
}
TEST_CASE("Blueprint preview renders exact cube and spaceship assets without scene instances",
          "[editor][opengl][blueprint][regression]") {
    using namespace editor_example;
    constexpr Extent2D extent{320, 240};
    const auto assets = std::filesystem::path(__FILE__).parent_path() / "../../examples/assets";
    auto cube = editor::EditableMesh::load(assets / "colored_cube.vmesh");
    REQUIRE(cube);
    State state{.document = {.mesh = std::move(*cube)}};
    const auto imported = import_mesh(state, assets / "spaceship.vmesh");
    REQUIRE(imported);
    const auto ship = find_instance(state, *imported)->blueprint;
    const auto ship_document = mesh_geometry(state, ship)->document();
    const auto original_cube = state.document.mesh.document();
    *instance_transform(state, *imported) = {{30, 40, 50}, {100, -130, 150}, 3};
    mesh_settings(state, *imported)->visible = false;
    mesh_settings(state, *imported)->brightness = 0;
    REQUIRE(key_property(state, {*imported, "visible"}, 0.F, false));
    REQUIRE(inspect_mesh(state, BlueprintId::mesh));
    REQUIRE(state.viewport.selected_object == *imported);

    auto window = test::create_hidden_opengl_window(extent.width, extent.height, "explicit blueprint preview");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto runtime = Runtime::create(*device, state);
    REQUIRE(runtime);
    const auto render = [&](bool diagnostic = false) {
        auto image = runtime->render(*device, state, extent, diagnostic);
        INFO((image ? "render complete" : image.error().message));
        REQUIRE(image);
        std::size_t lit{};
        for (std::size_t i = 0; i < image->pixels.size(); i += 4)
            if (std::to_integer<unsigned>(image->pixels[i]) > 80 ||
                std::to_integer<unsigned>(image->pixels[i + 1]) > 80 ||
                std::to_integer<unsigned>(image->pixels[i + 2]) > 80) ++lit;
        REQUIRE(lit > 100);
        return std::move(*image);
    };
    const auto provenance = [&](BlueprintId blueprint) {
        (void)render(true);
        const auto* manifest = runtime->diagnostic_manifest();
        REQUIRE(manifest);
        REQUIRE(manifest->items().size() == 1);
        const auto& item = manifest->items().front();
        CHECK_FALSE(item.provenance.entity); // an asset is not an invented entity
        CHECK(item.provenance.mesh == analysis::MeshAssetId{static_cast<u32>(blueprint)});
        CHECK(item.provenance.mesh_revision == analysis::AssetRevision{state.document.revision});
        CHECK(item.provenance.object_to_world == Mat4::identity());
        CHECK(item.primitives->size() == mesh_geometry(state, blueprint)->document().faces.size());
    };
    const auto cube_image = render();
    provenance(BlueprintId::mesh);
    REQUIRE(inspect_mesh(state, ship));
    const auto ship_image = render();
    CHECK(ship_image.pixels != cube_image.pixels);
    CHECK(state.viewport.selected_object == *imported);
    provenance(ship);

    // Deleting every instance must leave the explicitly inspected asset and
    // its cached GPU allocation available without manufacturing a replacement.
    while (!state.document.instances.empty()) REQUIRE(erase_instance(state, state.document.instances.back().id));
    CHECK(state.viewport.selected_object == 0);
    CHECK(state.viewport.inspected_mesh == ship);
    REQUIRE(runtime->update_mesh(*device, state));
    CHECK(render().pixels == ship_image.pixels);
    provenance(ship);
    CHECK(runtime->stats().resident_meshes == 2);
    CHECK(runtime->stats().full_mesh_uploads == 2);
    CHECK(runtime->stats().vertex_updates == 0);

    REQUIRE(inspect_mesh(state, BlueprintId::mesh));
    CHECK(render().pixels == cube_image.pixels);
    // Change a visible front-face vertex; the ship's mesh and pixels stay exact.
    const VertexEdit edit{state.document.revision, state.document.revision + 1, {{2, {1.2F, 1.2F, 1.2F}}}, 1};
    REQUIRE(apply_edit(state, edit));
    REQUIRE(runtime->update_mesh(*device, state));
    const auto changed_cube = render();
    CHECK(changed_cube.pixels != cube_image.pixels);
    CHECK(mesh_geometry(state, ship)->document() == ship_document);
    CHECK(state.document.mesh.document() != original_cube);
    REQUIRE(inspect_mesh(state, ship));
    CHECK(render().pixels == ship_image.pixels);
    provenance(ship);
    CHECK(state.document.instances.empty());
    CHECK(runtime->stats().resident_meshes == 2);
    CHECK(runtime->stats().full_mesh_uploads == 2);
    CHECK(runtime->stats().vertex_updates == 1);
    CHECK(runtime->stats().vertex_bytes_uploaded == 16);
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("Editor instances reuse one mesh allocation and survive deleting the final instance",
          "[editor][opengl][instances]") {
    using namespace editor_example;
    auto window = test::create_hidden_opengl_window(320, 240, "editor instances");
    if (!window) std::exit(77);
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto state = make_state();
    state.viewport.mode = ViewMode::scene;
    REQUIRE(erase_instance(state, 1));
    REQUIRE(erase_instance(state, 2));
    auto runtime = Runtime::create(*device, state);
    REQUIRE(runtime);
    const auto empty = runtime->render(*device, state, {320, 240}, false);
    REQUIRE(empty);
    auto first = instantiate(state, BlueprintId::mesh);
    REQUIRE(first);
    instance_transform(state, *first)->position = {-1, 0, 0};
    auto second = instantiate(state, BlueprintId::mesh);
    REQUIRE(second);
    instance_transform(state, *second)->position = {1, 0, 0};
    REQUIRE(runtime->update_mesh(*device, state));
    const auto both = runtime->render(*device, state, {320, 240}, false);
    REQUIRE(both);
    CHECK(both->pixels != empty->pixels);
    REQUIRE(erase_instance(state, *first));
    const auto one = runtime->render(*device, state, {320, 240}, false);
    REQUIRE(one);
    CHECK(one->pixels != both->pixels);
    state.viewport.selected_object = *second;
    const auto diagnostic = runtime->render(*device, state, {320, 240}, true);
    REQUIRE(diagnostic);
    CHECK(diagnostic->pixels != empty->pixels);
    CHECK(runtime->diagnostics().find("#" + std::to_string(*second)) != std::string_view::npos);
    REQUIRE(erase_instance(state, *second));
    const auto deleted = runtime->render(*device, state, {320, 240}, false);
    REQUIRE(deleted);
    CHECK(deleted->pixels == empty->pixels);
    CHECK(runtime->stats().full_mesh_uploads == 1);
    CHECK(runtime->stats().vertex_updates == 0);
    const auto recreated = instantiate(state, BlueprintId::mesh);
    REQUIRE(recreated);
    CHECK(*recreated > *second);
    REQUIRE(runtime->render(*device, state, {320, 240}, false));
    CHECK(runtime->stats().full_mesh_uploads == 1);
}

TEST_CASE("Instance transform Apply changes scene pixels without editing shared blueprint geometry",
          "[editor][opengl][instances][transform][regression]") {
    using namespace editor_example;
    constexpr Extent2D extent{320, 240};
    auto window = test::create_hidden_opengl_window(extent.width, extent.height, "instance transform Apply");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto state = make_state();
    state.viewport.mode = ViewMode::scene;
    state.viewport.editor_camera.distance = 6.F;
    state.viewport.time = 0.F;
    sun_settings(state, 2)->visible = false;
    *instance_transform(state, 1) = {{-1.2F, 0, 0}, {}, .5F};
    const auto sibling = instantiate(state, BlueprintId::mesh);
    REQUIRE(sibling);
    *instance_transform(state, *sibling) = {{1.2F, 0, 0}, {}, .5F};
    const auto original_sibling = *find_instance(state, *sibling);
    const auto original_geometry = state.document.mesh.document();
    const auto* original_positions =
        std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields.front().values).data();
    auto runtime = Runtime::create(*device, state);
    REQUIRE(runtime);
    const auto uploads = runtime->stats().full_mesh_uploads;
    REQUIRE(uploads == 1);
    REQUIRE(runtime->stats().resident_meshes == 1);

    const auto render = [&] {
        auto image = runtime->render(*device, state, extent, false);
        INFO((image ? "rendered" : image.error().message));
        REQUIRE(image);
        return std::move(*image);
    };
    const auto same_half = [&](const gfx::ImageData& a, const gfx::ImageData& b, bool left) {
        for (u32 y = 0; y < extent.height; ++y) {
            const auto first = (static_cast<std::size_t>(y) * extent.width +
                                (left ? 0 : extent.width / 2)) * 4;
            const auto last = first + extent.width * 2;
            if (!std::equal(a.pixels.begin() + static_cast<std::ptrdiff_t>(first),
                            a.pixels.begin() + static_cast<std::ptrdiff_t>(last),
                            b.pixels.begin() + static_cast<std::ptrdiff_t>(first))) return false;
        }
        return true;
    };
    const auto left_coverage = [&](const gfx::ImageData& image) {
        std::size_t pixels{};
        for (u32 y = 0; y < extent.height; ++y)
            for (u32 x = 0; x < extent.width / 2; ++x) {
                const auto at = (static_cast<std::size_t>(y) * extent.width + x) * 4;
                pixels += std::to_integer<unsigned>(image.pixels[at]) > 100U;
            }
        return pixels;
    };
    const auto apply = [&](std::string_view field_name, editor::Value value) {
        state.viewport.selected_object = 1;
        ProjectControls controls{state};
        editor::Inspector inspector{{1, 7, state.document.revision}};
        controls.describe_editor(inspector);
        const auto& schema = inspector.schema();
        const auto group = std::ranges::find_if(schema.controls, [&](const auto& control) {
            return control.kind == editor::Kind::group &&
                   std::ranges::any_of(control.fields, [&](const auto& field) { return field.key == field_name; });
        });
        INFO("Missing instance transform field " << field_name);
        REQUIRE(group != schema.controls.end());
        editor::Event event{schema.stamp, group->key, editor::Phase::apply, {}};
        for (const auto& field : group->fields)
            event.values.push_back({field.key, field.key == field_name ? value : field.value});
        auto applied=inspector.dispatch(event);
        INFO((applied ? "Applied instance transform" : applied.error().message));
        REQUIRE(applied);
        state.document.revision = inspector.schema().stamp.revision;
        // The generic worker path also checks mesh updates after callbacks.
        // Merely changing an instance transform must not upload any geometry.
        REQUIRE(runtime->update_mesh(*device, state));
    };
    const auto unchanged_storage = [&] {
        CHECK(state.document.mesh.document() == original_geometry);
        CHECK(std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields.front().values).data() ==
              original_positions);
        CHECK(*find_instance(state, *sibling) == original_sibling);
        CHECK(runtime->stats().resident_meshes == 1);
        CHECK(runtime->stats().full_mesh_uploads == uploads);
        CHECK(runtime->stats().vertex_updates == 0);
        CHECK(runtime->stats().vertex_bytes_uploaded == 0);
    };

    const auto small = render();
    REQUIRE(left_coverage(small) > 50);
    apply("scale", .9F);
    CHECK(instance_transform(state, 1)->scale == .9F);
    const auto large = render();
    CHECK_FALSE(same_half(small, large, true));
    CHECK(same_half(small, large, false));
    CHECK(left_coverage(large) > left_coverage(small) * 2);
    unchanged_storage();

    // With animation, Apply changes the key at the playhead rather than a
    // hidden base value that would be overridden again by evaluation.
    apply("scale", .5F);
    REQUIRE(key_property(state, {1, "scale"}, 0.F, .5F));
    REQUIRE(key_property(state, {1, "scale"}, 4.F, .5F));
    REQUIRE(add_keyframe(state,2.F));
    state.viewport.time = 2.F;
    const auto animated_small = render();
    CHECK(animated_small.pixels == small.pixels);
    apply("scale", .9F);
    CHECK(instance_transform(state, 1)->scale == .5F);
    CHECK(evaluate_instance(state, *find_instance(state, 1), 2.F).transform.scale == .9F);
    const auto animated_large = render();
    CHECK(animated_large.pixels == large.pixels);
    CHECK(same_half(animated_small, animated_large, false));
    unchanged_storage();

    state.document.timeline = {};
    state.viewport.time=0.F;
    apply("scale", .9F);
    apply("rotation", Vec3{0, 0, 90});
    const auto rotated = render();
    CHECK_FALSE(same_half(large, rotated, true));
    CHECK(same_half(large, rotated, false));
    unchanged_storage();

    // Blueprint inspection is local geometry, not a second view of the
    // selected instance's placement. Every component of its transform is ignored.
    state.viewport.mode = ViewMode::mesh;
    const auto isolated = render();
    *instance_transform(state, 1) = {{25, -13, 8}, {35, -70, 145}, 2.8F};
    REQUIRE(runtime->update_mesh(*device, state));
    const auto isolated_after = render();
    CHECK(isolated_after.pixels == isolated.pixels);
    CHECK(mesh_transform(ViewMode::mesh, *instance_transform(state, 1)) == Mat4::identity());
    REQUIRE(left_coverage(isolated) > 50);
    auto& hidden_appearance = *mesh_settings(state, 1);
    hidden_appearance.visible = false;
    hidden_appearance.brightness = 0.F;
    hidden_appearance.wireframe = true;
    REQUIRE(key_property(state, {1, "visible"}, 0.F, false));
    REQUIRE(key_property(state, {1, "brightness"}, 0.F, 0.F));
    REQUIRE(key_property(state, {1, "wireframe"}, 0.F, true));
    REQUIRE(runtime->update_mesh(*device, state));
    for (const auto time : {0.F, 2.F, 4.F}) {
        state.viewport.time = time;
        const auto isolated_hidden_instance = render();
        CHECK(isolated_hidden_instance.pixels == isolated.pixels);
    }
    CHECK_FALSE(mesh_settings(state, 1)->visible);
    CHECK(mesh_settings(state, 1)->brightness == 0.F);
    CHECK(mesh_settings(state, 1)->wireframe);
    unchanged_storage();
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("Editor runtime mesh update benchmark", "[editor][opengl][benchmark]") {
    auto window = test::create_hidden_opengl_window(640, 480, "editor runtime benchmark");
    if (!window) {
        std::cerr << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto state = make_state();
    const auto authoring_time=state.viewport.time;
    auto runtime = editor_example::Runtime::create(*device, state);
    INFO((runtime ? "runtime ready" : runtime.error().message));
    REQUIRE(runtime);
    CHECK_FALSE(runtime->readback(*device));
    CHECK_FALSE(runtime->present(*device));
    REQUIRE(runtime->render(*device, state, {640, 480}, false));
    CHECK(runtime->stats().full_mesh_uploads == 1);
    CHECK(runtime->stats().readbacks == 1);
    constexpr int repetitions = 64;
    const auto update_start = Clock::now();
    for (int i = 0; i < repetitions; ++i) {
        REQUIRE(state.document.mesh.set_position(0, {-.8F + static_cast<f32>(i % 2) * .1F, -.7F, 0}));
        REQUIRE(runtime->update_mesh(*device, state));
    }
    const auto update_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - update_start).count() /
        repetitions;
    CHECK(runtime->stats().full_mesh_uploads == 1);
    CHECK(runtime->stats().vertex_updates == 63);
    CHECK(runtime->stats().vertex_bytes_uploaded == 63 * 16);
    const auto render_start = Clock::now();
    for (int i = 0; i < 8; ++i)
        REQUIRE(runtime->render(*device, state, {640, 480}, false));
    const auto render_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - render_start).count() / 8;
    const auto captures = runtime->stats().readbacks;
    const auto frames = runtime->stats().rendered_frames;
    const auto native_start = Clock::now();
    for (int i = 0; i < 16; ++i) {
        REQUIRE(runtime->render_frame(*device, state, {640, 480}, 4.0F));
        REQUIRE(runtime->present(*device));
    }
    glFinish(); // Benchmark completion only; production render_frame/present do not synchronize.
    const auto native_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - native_start).count() / 16;
    CHECK(runtime->stats().readbacks == captures);
    CHECK(runtime->stats().rendered_frames == frames + 16);
    CHECK(runtime->stats().presented_frames == 16);
    CHECK(state.viewport.time == authoring_time);
    auto full = runtime->readback(*device);
    REQUIRE(full);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    glEnable(GL_FRAMEBUFFER_SRGB);
    auto small = runtime->readback(*device, {320, 240});
    REQUIRE(small);
    CHECK(glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE);
    CHECK(glIsEnabled(GL_FRAMEBUFFER_SRGB) == GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    CHECK(small->extent == Extent2D{320, 240});
    CHECK(small->pixels.size() == 320 * 240 * 4);
    CHECK(runtime->stats().preview_rescales == 1);
    CHECK(runtime->stats().rendered_frames == frames + 16);
    const auto center = [](const gfx::ImageData& image) {
        const auto i = (static_cast<std::size_t>(image.extent.height / 2) * image.extent.width +
                        image.extent.width / 2) *
                       4;
        return std::to_integer<int>(image.pixels[i]);
    };
    CHECK(center(*full) > 120);
    GLint saved_read_framebuffer{}, saved_read_buffer{};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read_framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &saved_read_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    std::array<unsigned char, 4> native_pixel{};
    glReadPixels(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, native_pixel.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(saved_read_framebuffer));
    glReadBuffer(static_cast<GLenum>(saved_read_buffer));
    CHECK(glGetError() == GL_NO_ERROR);
    CHECK(std::abs(static_cast<int>(native_pixel[0]) - center(*full)) <= 1);
    CHECK(native_pixel[0] > 120);
    CHECK(native_pixel[3] == 255);
    CHECK(std::abs(center(*full) - center(*small)) <= 1);
    CHECK_FALSE(runtime->readback(*device, {0, 240}));

    // Colors require decoding but can still reuse the typed vertex allocation.
    auto document = state.document.mesh.document();
    document.vertex_fields.push_back({"color/0",
                                      {content::vmesh::ScalarType::Float32, 4},
                                      std::vector<f32>{1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1}});
    auto colored = editor::EditableMesh::create(document);
    REQUIRE(colored);
    state.document.mesh = std::move(*colored);
    REQUIRE(runtime->update_mesh(*device, state));
    CHECK(runtime->stats().full_mesh_uploads == 1);
    auto red = runtime->render(*device, state, {640, 480}, false);
    REQUIRE(red);
    CHECK(red->pixels != full->pixels);
    document.faces = {{0, 2, 1}};
    auto reordered = editor::EditableMesh::create(document);
    REQUIRE(reordered);
    state.document.mesh = std::move(*reordered);
    REQUIRE(runtime->update_mesh(*device, state));
    CHECK(runtime->stats().full_mesh_uploads == 2);
    document.metadata["name"] = "metadata-only";
    document.edges = std::vector<gfx::Edge>{{0, 1}};
    auto annotated = editor::EditableMesh::create(std::move(document));
    REQUIRE(annotated);
    state.document.mesh = std::move(*annotated);
    REQUIRE(runtime->update_mesh(*device, state));
    CHECK(runtime->stats().full_mesh_uploads == 2);
    std::cout << "EDITOR_RUNTIME_BENCH update_ms=" << update_ms
              << " render_readback_ms=" << render_ms << " gpu_render_present_ms=" << native_ms
              << " gpu_only_readbacks=0\n";
}

TEST_CASE("Editor preview resolution retains mesh storage and reports frame transfer cost",
          "[editor][opengl][benchmark]") {
    auto window = test::create_hidden_opengl_window(64, 64, "editor resolution benchmark");
    if (!window) {
        std::cerr << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto state = make_state();
    auto runtime = editor_example::Runtime::create(*device, state);
    INFO((runtime ? "runtime ready" : runtime.error().message));
    REQUIRE(runtime);
    const auto* positions =
        std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data();
    for (const auto mode : {editor_example::ViewMode::mesh, editor_example::ViewMode::scene}) {
        state.viewport.mode = mode;
        for (const auto extent : {Extent2D{640, 480}, Extent2D{1500, 1100}, Extent2D{640, 480}}) {
            INFO("Preview extent " << extent.width << 'x' << extent.height);
            // Exclude resource allocation and first-use driver compilation from
            // steady-state numbers. Timings are informative, never test thresholds.
            REQUIRE(runtime->render(*device, state, extent, false));
            constexpr int repetitions = 12;
            double submit_ms{}, transfer_ms{};
            for (int i = 0; i < repetitions; ++i) {
                const auto time = 3.F + static_cast<f32>(i) / 60.F;
                REQUIRE(runtime->render_frame(*device, state, extent, time));
                auto image = runtime->readback(*device);
                REQUIRE(image);
                CHECK(image->extent == extent);
                CHECK(image->pixels.size() ==
                      static_cast<std::size_t>(extent.width) * extent.height * 4);
                submit_ms += runtime->stats().last_render_ms;
                transfer_ms += runtime->stats().last_readback_ms;
            }
            std::cout << "EDITOR_RESOLUTION_BENCH mode="
                      << (mode == editor_example::ViewMode::mesh ? "mesh" : "scene")
                      << " extent=" << extent.width << 'x' << extent.height
                      << " submit_ms=" << submit_ms / repetitions
                      << " readback_ms=" << transfer_ms / repetitions << '\n';
            CHECK(runtime->stats().full_mesh_uploads == 1);
            CHECK(runtime->stats().vertex_updates == 0);
            CHECK(runtime->stats().preview_rescales == 0);
            CHECK(
                std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data() ==
                positions);
        }
    }
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("Sun softening preserves crisp mesh edges and original depth occlusion",
          "[editor][opengl][rendering]") {
    constexpr Extent2D extent{192, 144};
    auto window = test::create_hidden_opengl_window(extent.width, extent.height,
                                                    "editor selective softening");
    if (!window) {
        std::cerr << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto state = make_state();
    state.viewport.mode = editor_example::ViewMode::scene;
    (*editor_example::instance_transform(state, 1)).position = {};
    (*editor_example::instance_transform(state, 1)).scale = .5F;
    (*editor_example::sun_settings(state, 2)).bloom = 0;
    (*editor_example::sun_settings(state, 2)).visible = false;
    auto runtime = editor_example::Runtime::create(*device, state);
    INFO((runtime ? "runtime ready" : runtime.error().message));
    REQUIRE(runtime);
    auto mesh = runtime->render(*device, state, extent, false, 0.F);
    REQUIRE(mesh);
    (*editor_example::sun_settings(state, 2)).visible = true;
    (*editor_example::instance_transform(state, 2)).position = {100, 100, 100};
    auto offscreen_sun = runtime->render(*device, state, extent, false, 0.F);
    REQUIRE(offscreen_sun);
    const auto differing_pixels = [](const auto& a, const auto& b) {
        std::size_t different{};
        for (std::size_t i = 0; i < a.pixels.size(); i += 4) {
            bool differs{};
            for (std::size_t channel = 0; channel < 3; ++channel)
                differs |= std::abs(std::to_integer<int>(a.pixels[i + channel]) -
                                    std::to_integer<int>(b.pixels[i + channel])) > 1;
            different += differs;
        }
        return different;
    };
    CHECK(differing_pixels(*mesh, *offscreen_sun) == 0);
    state.viewport.selected_object = 0;
    auto unselected_diagnostic = runtime->render(*device, state, extent, true, 0.F);
    REQUIRE(unselected_diagnostic);
    CHECK(differing_pixels(*mesh, *unselected_diagnostic) == 0);

    // Reusing the sun's depth must preserve both sides of opaque occlusion,
    // including after targets are recreated by a viewport resize.
    (*editor_example::instance_transform(state, 2)).position = {};
    (*editor_example::sun_settings(state, 2)).radius = 1.5F;
    (*editor_example::mesh_settings(state, 1)).visible = false;
    for (const auto size : {extent, Extent2D{320, 240}, extent}) {
        auto sun_only = runtime->render(*device, state, size, false, 0.F);
        REQUIRE(sun_only);
        (*editor_example::mesh_settings(state, 1)).visible = true;
        (*editor_example::instance_transform(state, 1)).position = {0, 0, -2};
        auto behind = runtime->render(*device, state, size, false, 0.F);
        REQUIRE(behind);
        CHECK(differing_pixels(*sun_only, *behind) == 0);
        (*editor_example::instance_transform(state, 1)).position = {0, 0, 2};
        auto in_front = runtime->render(*device, state, size, false, 0.F);
        REQUIRE(in_front);
        CHECK(differing_pixels(*sun_only, *in_front) > 20);
        (*editor_example::mesh_settings(state, 1)).visible = false;
    }
    CHECK(runtime->stats().full_mesh_uploads == 1);
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("Editor optional surface attributes provide lighting and HDR engine emission",
          "[editor][opengl][fleet]") {
    using namespace editor_example;
    constexpr Extent2D extent{192,144};
    auto window = test::create_hidden_opengl_window(extent.width, extent.height, "fleet mesh shading");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto state = make_state();
    state.viewport.editor_camera.distance = 4;
    auto runtime = Runtime::create(*device, state);
    REQUIRE(runtime);
    const auto render = [&] {
        auto image = runtime->render(*device, state, extent, false);
        INFO((image ? "rendered" : image.error().message));
        REQUIRE(image);
        return std::move(*image);
    };
    const auto center = (static_cast<std::size_t>(extent.height / 2) * extent.width + extent.width / 2) * 4;
    const auto unlit = render();
    CHECK(unlit.pixels[center] == unlit.pixels[center + 2]);
    auto document = state.document.mesh.document();
    document.vertex_fields.push_back({"normal", {content::vmesh::ScalarType::Float32, 3},
        std::vector<f32>{0,0,1, 0,0,1, 0,0,1}});
    document.vertex_fields.push_back({"emission", {content::vmesh::ScalarType::Float32, 1},
        std::vector<f32>{0,0,0}});
    const auto authored = document;
    auto lit_mesh = editor::EditableMesh::create(document);
    REQUIRE(lit_mesh);
    state.document.mesh = std::move(*lit_mesh);
    REQUIRE(runtime->update_mesh(*device, state));
    const auto lit = render();
    CHECK(std::to_integer<int>(lit.pixels[center]) > std::to_integer<int>(lit.pixels[center + 2]) + 10);
    CHECK(runtime->stats().full_mesh_uploads == 1);
    CHECK(state.document.mesh.document() == authored);

    // A normals-only edit must upload just the changed surface record, including
    // its nonzero offset; the position/color stream stays untouched.
    const auto surface_bytes = runtime->stats().vertex_bytes_uploaded;
    std::get<std::vector<f32>>(document.vertex_fields[1].values)[6] = 1.F;
    auto tilted = editor::EditableMesh::create(document);
    REQUIRE(tilted);
    state.document.mesh = std::move(*tilted);
    const std::array affected{BlueprintId::mesh};
    REQUIRE(runtime->update_mesh(*device, state, affected));
    CHECK(runtime->stats().vertex_bytes_uploaded == surface_bytes + 16);
    CHECK(runtime->stats().full_mesh_uploads == 1);
    CHECK(render().pixels != lit.pixels);

    auto restored = editor::EditableMesh::create(authored);
    REQUIRE(restored);
    state.document.mesh = std::move(*restored);
    REQUIRE(runtime->update_mesh(*device, state, affected));
    CHECK(runtime->stats().vertex_bytes_uploaded == surface_bytes + 32);
    CHECK(render().pixels == lit.pixels);
    document = authored;

    document.vertex_fields.back().values = std::vector<f32>{30,30,30};
    auto emissive = editor::EditableMesh::create(document);
    REQUIRE(emissive);
    state.document.mesh = std::move(*emissive);
    REQUIRE(runtime->update_mesh(*device, state));
    const auto glow_off = render();
    state.document.environment.bloom_threshold = 1;
    state.document.environment.bloom_strength = .35F;
    const auto glow_on = render();
    std::size_t halo_pixels{};
    for (std::size_t i = 0; i < glow_off.pixels.size(); i += 4)
        if (std::to_integer<int>(glow_off.pixels[i]) < 30 &&
            std::to_integer<int>(glow_on.pixels[i]) > std::to_integer<int>(glow_off.pixels[i]) + 5)
            ++halo_pixels;
    CHECK(halo_pixels > 20);
    CHECK(runtime->stats().full_mesh_uploads == 1);

    const auto bytes = runtime->stats().vertex_bytes_uploaded;
    const std::array positions{VertexPosition{2, {0, .6F, 0}}};
    REQUIRE(runtime->update_positions(*device, BlueprintId::mesh, positions));
    CHECK(runtime->stats().vertex_bytes_uploaded == bytes + 16);
    REQUIRE(runtime->render(*device, state, extent, true));
    const auto* manifest = runtime->diagnostic_manifest();
    REQUIRE(manifest);
    REQUIRE(manifest->items().size() == 1);
    CHECK(manifest->items()[0].primitives->sources()[0].vertices == gfx::TriangleFace{0,1,2});
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("Nonuniform instance scale matches baked geometry and inverse-transpose normals", "[editor][opengl][axis-scale]") {
    using namespace editor_example;
    constexpr Extent2D extent{192,144};
    auto window=test::create_hidden_opengl_window(extent.width,extent.height,"Instance axis scale");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto access=window->make_current();REQUIRE(access);
    auto device=opengl::Device::create(*access);REQUIRE(device);
    auto state=make_state();state.viewport.mode=ViewMode::scene;state.viewport.editor_camera.distance=5;
    sun_settings(state,2)->visible=false;
    auto document=state.document.mesh.document();
    document.vertex_fields.push_back({"normal",{content::vmesh::ScalarType::Float32,3},
        std::vector<f32>{.6F,0,.8F,.6F,0,.8F,.6F,0,.8F}});
    auto mesh=editor::EditableMesh::create(document);REQUIRE(mesh);state.document.mesh=std::move(*mesh);
    *instance_transform(state,1)={{},{10,20,0},1,{1.8F,.6F,1}};
    auto runtime=Runtime::create(*device,state);REQUIRE(runtime);
    auto stretched=runtime->render(*device,state,extent,false);REQUIRE(stretched);
    CHECK(pick_object(state,{.5F,.5F},extent)==1);
    CHECK(runtime->stats().full_mesh_uploads==1);
    const auto axes=instance_transform(state,1)->axis_scale;
    auto& positions=std::get<std::vector<f32>>(document.vertex_fields[0].values);
    auto& normals=std::get<std::vector<f32>>(document.vertex_fields[1].values);
    for(std::size_t i=0;i<positions.size();++i){positions[i]*=axes[i%3];normals[i]/=axes[i%3];}
    mesh=editor::EditableMesh::create(document);REQUIRE(mesh);state.document.mesh=std::move(*mesh);
    instance_transform(state,1)->axis_scale={1,1,1};
    REQUIRE(runtime->update_mesh(*device,state));
    auto baked=runtime->render(*device,state,extent,false);REQUIRE(baked);
    REQUIRE(stretched->pixels.size()==baked->pixels.size());
    int worst{};
    for(std::size_t i=0;i<baked->pixels.size();++i)
        worst=std::max(worst,std::abs(std::to_integer<int>(stretched->pixels[i])-std::to_integer<int>(baked->pixels[i])));
    CHECK(worst<=1);
    CHECK(glGetError()==GL_NO_ERROR);
}

TEST_CASE("Editor stars are stable world directions rather than a camera-attached overlay",
          "[editor][opengl][fleet]") {
    using namespace editor_example;
    constexpr Extent2D extent{256,192};
    auto window = test::create_hidden_opengl_window(extent.width, extent.height, "fleet stars");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto state = make_state();
    state.viewport.mode = ViewMode::scene;
    mesh_settings(state, 1)->visible = false;
    sun_settings(state, 2)->visible = false;
    state.document.environment.stars = 4000;
    auto runtime = Runtime::create(*device, state);
    REQUIRE(runtime);
    const auto render = [&] {
        auto image = runtime->render(*device, state, extent, false);
        INFO((image ? "rendered" : image.error().message));
        REQUIRE(image);
        return std::move(*image);
    };
    const auto original = render();
    CHECK(original.pixels == render().pixels);
    state.viewport.editor_camera.target = {8,1,-5};
    const auto translated = render();
    std::size_t translation_difference{};
    for (std::size_t i = 0; i < original.pixels.size(); ++i)
        translation_difference += std::abs(std::to_integer<int>(original.pixels[i]) -
            std::to_integer<int>(translated.pixels[i])) > 1;
    CHECK(translation_difference < 10);
    state.viewport.editor_camera.yaw = 25;
    CHECK(render().pixels != original.pixels);
    state.viewport.editor_camera = {.yaw=0,.pitch=0,.distance=8};
    state.document.environment.star_seed = 77;
    CHECK(render().pixels != original.pixels);
    state.document.environment.stars = 0;
    const auto empty = render();
    CHECK(empty.pixels != original.pixels);
    CHECK(runtime->stats().full_mesh_uploads == 1);
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("Authored fleet reveal occludes the formation before revealing it with the timeline camera",
          "[editor][opengl][fleet][scene]") {
    using namespace editor_example;
    constexpr Extent2D extent{480,300};
    const auto asset = std::filesystem::path(__FILE__).parent_path() /
        "../../examples/assets/fleet_reveal.vscene";
    auto loaded = load_scene(asset);
    INFO((loaded ? "scene loaded" : loaded.error().message));
    REQUIRE(loaded);
    auto state = std::move(*loaded);
    REQUIRE(state.document.instances.size() >= 12);
    REQUIRE(state.document.mesh_assets.size() >= 3);
    // The file predates camera instances; its animated shot loads as one.
    const auto* shot = active_camera(state, 0);
    REQUIRE(shot);
    CHECK(shot->name == "Animation camera");
    REQUIRE(state.document.timeline.find({shot->id, "position"}));
    state.viewport.mode = ViewMode::scene;
    auto window = test::create_hidden_opengl_window(extent.width, extent.height, "authored fleet reveal");
    if (!window) { std::cerr << window.error().message << '\n'; std::exit(77); }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto runtime = Runtime::create(*device, state);
    INFO((runtime ? "runtime ready" : runtime.error().message));
    REQUIRE(runtime);
    const auto render = [&](f32 time) {
        const auto view = camera(*evaluate_camera(state, time), ViewMode::scene);
        auto image = runtime->render(*device, {state, view, extent, time, false});
        INFO((image ? "rendered" : image.error().message));
        REQUIRE(image);
        return std::move(*image);
    };
    const auto fleet_visible = [&](bool visible) {
        for (auto& instance : state.document.instances)
            if (auto* mesh = std::get_if<MeshSettings>(&instance.settings); mesh && instance.id > 2)
                mesh->visible = visible;
    };
    const auto changed_pixels = [](const gfx::ImageData& a, const gfx::ImageData& b) {
        std::size_t count{};
        for (std::size_t i = 0; i < a.pixels.size(); i += 4) {
            bool changed{};
            for (std::size_t c = 0; c < 3; ++c)
                changed |= std::abs(std::to_integer<int>(a.pixels[i+c]) -
                    std::to_integer<int>(b.pixels[i+c])) > 1;
            count += changed;
        }
        return count;
    };
    const auto opening = render(0);
    fleet_visible(false);
    const auto opening_without_fleet = render(0);
    CHECK(changed_pixels(opening, opening_without_fleet) == 0);
    const auto reveal_without_fleet = render(26);
    fleet_visible(true);
    const auto reveal = render(26);
    const auto visible_pixels = changed_pixels(reveal, reveal_without_fleet);
    INFO("Fleet contributes " << visible_pixels << " pixels in the reveal");
    CHECK(visible_pixels > 400);
    CHECK(changed_pixels(opening, reveal) > 2000);

    const auto explicit_camera = camera(*evaluate_camera(state, 26), ViewMode::scene);
    state.viewport.selected_object = 3; // BASTION / flagship, a carrier instance.
    auto diagnostic = runtime->render(*device, {state, explicit_camera, extent, 26, true});
    REQUIRE(diagnostic);
    const auto* manifest = runtime->diagnostic_manifest();
    REQUIRE(manifest);
    REQUIRE(manifest->items().size() == 1);
    CHECK(manifest->items()[0].provenance.entity == analysis::EntityId{3});
    CHECK(manifest->items()[0].provenance.mesh == analysis::MeshAssetId{3});
    CHECK(manifest->items()[0].primitives->size() > 1000);
    std::size_t selected_pixels{};
    for (std::size_t i = 0; i < diagnostic->pixels.size(); i += 4)
        selected_pixels += std::to_integer<int>(diagnostic->pixels[i]) >= 50;
    CHECK(selected_pixels > 100);
    CHECK(runtime->stats().resident_meshes == 4);
    CHECK(runtime->stats().full_mesh_uploads == 4);
    CHECK(runtime->stats().vertex_updates == 0);
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("Dynamic mesh record updates retain vertex buffers and inferred VAOs",
          "[editor][opengl][mesh]") {
    struct Position : gfx::Semantic<Vec2> {};
    struct Tint : gfx::Semantic<Vec4> {};
    using PositionRecord = gfx::Record<Position>;
    using TintRecord = gfx::Record<Tint>;
    using Inputs = shader::VertexInputs<Position, Tint>;
    auto window = test::create_hidden_opengl_window(32, 32, "dynamic mesh");
    if (!window) {
        std::cerr << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    gfx::Mesh<PositionRecord, TintRecord> mesh(3);
    mesh.vertices<PositionRecord>()[0].set(Position{}, Vec2{-.5F, -.5F});
    mesh.vertices<PositionRecord>()[1].set(Position{}, Vec2{.5F, -.5F});
    mesh.vertices<PositionRecord>()[2].set(Position{}, Vec2{0, .5F});
    mesh.add_face(0, 1, 2);
    auto gpu = opengl::upload_mesh(*device, mesh, {.dynamic_vertices = true});
    REQUIRE(gpu);
    REQUIRE(gpu->prepare_vertex_input<Inputs>(*device));
    using Outputs = shader::VertexOutputs<shader::ClipPosition, shader::smooth<Tint>>;
    using FragmentInputs = shader::FragmentInputs<shader::smooth<Tint>>;
    using FragmentOutputs = shader::FragmentOutputs<shader::Color<0>>;
    auto vs = shader::vertex<Inputs, Outputs>([](auto& s) {
        return s.output(
            dsl::field<shader::ClipPosition>(dsl::vec4(s.input(Position{}), 0.0F, 1.0F)),
            dsl::field<Tint>(s.input(Tint{})));
    });
    auto fs = shader::fragment<FragmentInputs, FragmentOutputs>(
        [](auto& s) { return s.output(dsl::field<shader::Color<0>>(s.input(Tint{}))); });
    REQUIRE(vs);
    REQUIRE(fs);
    auto neutral = shader::link(*vs, *fs);
    REQUIRE(neutral);
    auto program = render::compile_program(*device, *neutral);
    REQUIRE(program);
    REQUIRE(gpu->bind_vertex_input(*device, *program));
    GLint vao{}, position_buffer{}, color_buffer{}, index_buffer{};
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetVertexArrayIndexediv(static_cast<GLuint>(vao), 0, GL_VERTEX_BINDING_BUFFER,
                              &position_buffer);
    glGetVertexArrayIndexediv(static_cast<GLuint>(vao), 1, GL_VERTEX_BINDING_BUFFER, &color_buffer);
    glGetVertexArrayiv(static_cast<GLuint>(vao), GL_ELEMENT_ARRAY_BUFFER_BINDING, &index_buffer);
    REQUIRE(vao > 0);
    REQUIRE(position_buffer > 0);
    REQUIRE(color_buffer > 0);
    REQUIRE(index_buffer > 0);
    CHECK(gpu->cached_vertex_input_count() == 1);
    const auto fingerprint = gpu->topology_fingerprint();
    gfx::VertexStream<PositionRecord> positions(1);
    positions[0].set(Position{}, Vec2{.75F, .25F});
    REQUIRE(gpu->update_vertices(*device, positions, 1));
    gfx::VertexStream<TintRecord> colors(3);
    for (auto& color : colors)
        color.set(Tint{}, Vec4{1, 0, 0, 1});
    REQUIRE(gpu->update_vertices(*device, colors));
    REQUIRE(gpu->prepare_vertex_input<Inputs>(*device));
    REQUIRE(gpu->bind_vertex_input(*device, *program));
    GLint after_vao{}, after_position{}, after_color{}, after_index{};
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &after_vao);
    glGetVertexArrayIndexediv(static_cast<GLuint>(after_vao), 0, GL_VERTEX_BINDING_BUFFER,
                              &after_position);
    glGetVertexArrayIndexediv(static_cast<GLuint>(after_vao), 1, GL_VERTEX_BINDING_BUFFER,
                              &after_color);
    glGetVertexArrayiv(static_cast<GLuint>(after_vao), GL_ELEMENT_ARRAY_BUFFER_BINDING,
                       &after_index);
    CHECK(after_vao == vao);
    CHECK(after_position == position_buffer);
    CHECK(after_color == color_buffer);
    CHECK(after_index == index_buffer);
    std::array<PositionRecord, 3> uploaded_positions;
    glGetNamedBufferSubData(static_cast<GLuint>(position_buffer), 0, sizeof(uploaded_positions),
                            uploaded_positions.data());
    CHECK(uploaded_positions[0].get(Position{}) == Vec2{-.5F, -.5F});
    CHECK(uploaded_positions[1].get(Position{}) == Vec2{.75F, .25F});
    CHECK(uploaded_positions[2].get(Position{}) == Vec2{0, .5F});
    std::array<TintRecord, 3> uploaded_colors;
    glGetNamedBufferSubData(static_cast<GLuint>(color_buffer), 0, sizeof(uploaded_colors),
                            uploaded_colors.data());
    CHECK(uploaded_colors[2].get(Tint{}) == Vec4{1, 0, 0, 1});
    CHECK(glGetError() == GL_NO_ERROR);
    CHECK(gpu->cached_vertex_input_count() == 1);
    CHECK(gpu->topology_fingerprint() == fingerprint);
    CHECK(gpu->vertex_count() == 3);
    CHECK_FALSE(gpu->update_vertices(*device, positions, 3));
    auto immutable = opengl::upload_mesh(*device, mesh);
    REQUIRE(immutable);
    CHECK_FALSE(immutable->update_vertices(*device, positions));
    auto framebuffer = opengl::RenderTarget::create(
        *device, {.color = gfx::ImageFormat::rgba8, .depth = false}, {32, 32});
    REQUIRE(framebuffer);
    auto frame = render::begin_frame(*device, *framebuffer,
                                     {.extent = {32, 32},
                                      .color_encoding = render::ColorEncoding::linear,
                                      .clear_color = {},
                                      .clear_depth = {}});
    REQUIRE(frame);
    CHECK_FALSE(gpu->update_vertices(*device, positions));
    REQUIRE(frame->end());
    auto moved = std::move(*gpu);
    CHECK_FALSE(gpu->update_vertices(*device, positions));
    REQUIRE(moved.update_vertices(*device, positions));
    CHECK(moved.cached_vertex_input_count() == 1);
}

TEST_CASE("Editor timeline samples match baked production and diagnostic renders without geometry "
          "uploads",
          "[editor][opengl][animation]") {
    using namespace editor_example;
    constexpr Extent2D extent{160, 120};
    auto window = test::create_hidden_opengl_window(extent.width, extent.height,
                                                    "timeline render verification");
    if (!window) {
        std::cerr << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto state = make_state();
    state.viewport.mode = ViewMode::scene;
    (*editor_example::instance_transform(state, 1)).position = {};
    (*editor_example::sun_settings(state, 2)).visible = false;
    state.viewport.time = 7; // Explicit overrides must not accidentally sample this authored playhead.
    REQUIRE(key_property(state, {1, "position"}, 0, Vec3{-1.2F, 0, 0}));
    REQUIRE(key_property(state, {1, "position"}, 4, Vec3{1.2F, 0, 0}));
    REQUIRE(key_property(state, {1, "scale"}, 0, .4F));
    REQUIRE(key_property(state, {1, "scale"}, 4, 1.3F));
    REQUIRE(key_property(state, {1, "brightness"}, 0, .1F));
    REQUIRE(key_property(state, {1, "brightness"}, 4, 2.F));
    REQUIRE(key_property(state, {1, "wireframe"}, 0, false));
    REQUIRE(key_property(state, {1, "wireframe"}, 3, true));
    REQUIRE(key_property(state, {1, "wireframe"}, 4, false));
    REQUIRE(key_property(state, {1, "visible"}, 0, true));
    REQUIRE(key_property(state, {1, "visible"}, 5, false));
    REQUIRE(key_property(state, {1, "visible"}, 6, true));
    const auto original = state;
    const auto* positions =
        std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data();
    auto runtime = Runtime::create(*device, state);
    INFO((runtime ? "runtime ready" : runtime.error().message));
    REQUIRE(runtime);
    const auto uploads = runtime->stats().full_mesh_uploads;
    const auto updates = runtime->stats().vertex_updates;
    const auto bytes = runtime->stats().vertex_bytes_uploaded;
    auto reference = state;
    reference.document.timeline = {};
    const auto baked = [&](f32 time) {
        const auto sampled = evaluate_scene(state, time);
        (*editor_example::mesh_settings(reference, 1)) = sampled.model;
        (*editor_example::sun_settings(reference, 2)) = sampled.sun;
        (*editor_example::instance_transform(reference, 1)) = sampled.model_transform;
        (*editor_example::instance_transform(reference, 2)) = sampled.sun_transform;
        reference.viewport.time = time;
    };
    struct Coverage {
        std::size_t count{};
        double x_sum{};
        double centroid() const { return x_sum / static_cast<double>(count); }
    };
    const auto coverage = [](const gfx::ImageData& image) {
        Coverage result;
        for (u32 y = 0; y < image.extent.height; ++y)
            for (u32 x = 0; x < image.extent.width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * image.extent.width + x) * 4;
                // Mesh is white; tone-mapped clear and diagnostic backgrounds
                // are below this threshold, while the source face is above it.
                if (std::to_integer<unsigned>(image.pixels[offset]) > 40U) {
                    ++result.count;
                    result.x_sum += x;
                }
            }
        return result;
    };
    Coverage first, last, diagnostic_first, diagnostic_last;
    for (const auto time : {0.F, 1.F, 2.F, 3.F, 4.F}) {
        INFO("mesh time override=" << time);
        REQUIRE(runtime->update_mesh(*device, state));
        baked(time);
        auto normal = runtime->render(*device, state, extent, false, time);
        REQUIRE(normal);
        auto normal_reference = runtime->render(*device, reference, extent, false);
        REQUIRE(normal_reference);
        CHECK(normal->pixels == normal_reference->pixels);
        auto diagnostic = runtime->render(*device, state, extent, true, time);
        REQUIRE(diagnostic);
        auto diagnostic_reference = runtime->render(*device, reference, extent, true);
        REQUIRE(diagnostic_reference);
        CHECK(diagnostic->pixels == diagnostic_reference->pixels);
        CHECK(runtime->diagnostics().find("Mesh source faces") != std::string_view::npos);
        if (time == 0) {
            first = coverage(*normal);
            diagnostic_first = coverage(*diagnostic);
        } else if (time == 4) {
            last = coverage(*normal);
            diagnostic_last = coverage(*diagnostic);
        }
    }
    REQUIRE(first.count > 10);
    REQUIRE(diagnostic_first.count > 10);
    CHECK(last.count > first.count * 4);
    CHECK(diagnostic_last.count > diagnostic_first.count * 4);
    CHECK(first.centroid() < extent.width * .5);
    CHECK(last.centroid() > extent.width * .5);
    CHECK(diagnostic_first.centroid() < extent.width * .5);
    CHECK(diagnostic_last.centroid() > extent.width * .5);
    CHECK(std::abs(first.centroid() - diagnostic_first.centroid()) < 1.0);
    CHECK(std::abs(last.centroid() - diagnostic_last.centroid()) < 1.0);

    auto hidden = runtime->render(*device, state, extent, false, 5.F);
    REQUIRE(hidden);
    CHECK(coverage(*hidden).count == 0);
    auto shown = runtime->render(*device, state, extent, false, 6.F);
    REQUIRE(shown);
    CHECK(coverage(*shown).count > 10);
    const auto readbacks = runtime->stats().readbacks;
    for (int i = 0; i < 13; ++i)
        REQUIRE(runtime->render_frame(*device, state, extent, static_cast<f32>(i) / 3.F));
    CHECK(runtime->stats().readbacks == readbacks);
    CHECK(runtime->stats().full_mesh_uploads == uploads);
    CHECK(runtime->stats().vertex_updates == updates);
    CHECK(runtime->stats().vertex_bytes_uploaded == bytes);
    CHECK(state.viewport.time == original.viewport.time);
    CHECK(state.document.revision == original.document.revision);
    CHECK((*editor_example::mesh_settings(state, 1)) == (*editor_example::mesh_settings(original, 1)));
    CHECK((*editor_example::sun_settings(state, 2)) == (*editor_example::sun_settings(original, 2)));
    CHECK((*editor_example::instance_transform(state, 1)) == (*editor_example::instance_transform(original, 1)));
    CHECK((*editor_example::instance_transform(state, 2)) == (*editor_example::instance_transform(original, 2)));
    CHECK(state.document.timeline == original.document.timeline);
    CHECK(state.document.mesh.document() == original.document.mesh.document());
    CHECK(std::get<std::vector<f32>>(state.document.mesh.document().vertex_fields[0].values).data() ==
          positions);
    CHECK_FALSE(
        runtime->render_frame(*device, state, extent, std::numeric_limits<f32>::quiet_NaN()));
    CHECK_FALSE(
        runtime->render(*device, state, extent, true, std::numeric_limits<f32>::infinity()));
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("Sun timeline parameters and diagnostic geometry honor the same explicit preview time",
          "[editor][opengl][animation]") {
    using namespace editor_example;
    constexpr Extent2D extent{128, 96};
    auto window =
        test::create_hidden_opengl_window(extent.width, extent.height, "sun timeline verification");
    if (!window) {
        std::cerr << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    REQUIRE(access);
    auto device = opengl::Device::create(*access);
    REQUIRE(device);
    auto state = make_state();
    state.viewport.mode = ViewMode::scene;
    state.viewport.selected_object = 2;
    (*editor_example::mesh_settings(state, 1)).visible = false;
    (*editor_example::sun_settings(state, 2)).visible = true;
    state.viewport.time = 7;
    REQUIRE(key_property(state, {2, "position"}, 0, Vec3{-.8F, 0, 0}));
    REQUIRE(key_property(state, {2, "position"}, 4, Vec3{.8F, 0, 0}));
    REQUIRE(key_property(state, {2, "radius"}, 0, .45F));
    REQUIRE(key_property(state, {2, "radius"}, 4, 1.F));
    REQUIRE(key_property(state, {2, "displacement"}, 0, 0.F));
    REQUIRE(key_property(state, {2, "displacement"}, 4, 1.F));
    REQUIRE(key_property(state, {2, "bloom"}, 0, 0.F));
    REQUIRE(key_property(state, {2, "bloom"}, 4, .7F));
    REQUIRE(key_property(state, {2, "white_spots"}, 0, false));
    REQUIRE(key_property(state, {2, "white_spots"}, 2, true));
    const auto original = state;
    auto runtime = Runtime::create(*device, state);
    INFO((runtime ? "runtime ready" : runtime.error().message));
    REQUIRE(runtime);
    auto reference = state;
    reference.document.timeline = {};
    std::vector<std::byte> first, first_diagnostic;
    for (const auto time : {0.F, 2.F, 4.F}) {
        INFO("sun time override=" << time);
        const auto sampled = evaluate_scene(state, time);
        (*editor_example::mesh_settings(reference, 1)) = sampled.model;
        (*editor_example::sun_settings(reference, 2)) = sampled.sun;
        (*editor_example::instance_transform(reference, 1)) = sampled.model_transform;
        (*editor_example::instance_transform(reference, 2)) = sampled.sun_transform;
        reference.viewport.time = time;
        auto normal = runtime->render(*device, state, extent, false, time);
        REQUIRE(normal);
        auto normal_reference = runtime->render(*device, reference, extent, false);
        REQUIRE(normal_reference);
        CHECK(normal->pixels == normal_reference->pixels);
        auto diagnostic = runtime->render(*device, state, extent, true, time);
        REQUIRE(diagnostic);
        auto diagnostic_reference = runtime->render(*device, reference, extent, true);
        REQUIRE(diagnostic_reference);
        CHECK(diagnostic->pixels == diagnostic_reference->pixels);
        CHECK(runtime->diagnostics().find("Sun surface / pre-bloom") != std::string_view::npos);
        if (time == 0) {
            first = normal->pixels;
            first_diagnostic = diagnostic->pixels;
        } else {
            CHECK(normal->pixels != first);
            CHECK(diagnostic->pixels != first_diagnostic);
        }
    }
    CHECK(runtime->stats().full_mesh_uploads == 1);
    CHECK(runtime->stats().vertex_updates == 0);
    CHECK(runtime->stats().vertex_bytes_uploaded == 0);
    CHECK(state.viewport.time == 7.F);
    CHECK((*editor_example::mesh_settings(state, 1)) == (*editor_example::mesh_settings(original, 1)));
    CHECK((*editor_example::sun_settings(state, 2)) == (*editor_example::sun_settings(original, 2)));
    CHECK((*editor_example::instance_transform(state, 1)) == (*editor_example::instance_transform(original, 1)));
    CHECK((*editor_example::instance_transform(state, 2)) == (*editor_example::instance_transform(original, 2)));
    CHECK(state.document.timeline == original.document.timeline);
    CHECK(state.document.mesh.document() == original.document.mesh.document());
    CHECK(glGetError() == GL_NO_ERROR);
}
