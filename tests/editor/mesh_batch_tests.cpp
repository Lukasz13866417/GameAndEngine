#include "../../examples/editor/blueprint_mesh_renderer.hpp"
#include "../../examples/editor/runtime.hpp"
#include "../support/glfw_opengl.hpp"
#include <vng/render/program.hpp>
#include <vng/gfx/buffer.hpp>
#include <vng/opengl/render_target.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glad/gl.h>
#include <cstdlib>
#include <iostream>
#include <map>

namespace {
using namespace vng;
using namespace editor_example;
using namespace mesh_shading;
static_assert(render::RendererFor<BlueprintMeshRenderer,opengl::Frame>);
// Count native indexed draws, not just our own renderer bookkeeping.
struct NativeDraws {
    static inline PFNGLDRAWELEMENTSINSTANCEDPROC previous{};
    static inline std::vector<GLsizei> counts;
    NativeDraws() {counts.clear();previous=glad_glDrawElementsInstanced;glad_glDrawElementsInstanced=draw;}
    ~NativeDraws() {glad_glDrawElementsInstanced=previous;}
    static void GLAD_API_PTR draw(GLenum mode,GLsizei count,GLenum type,const void* indices,GLsizei instances) {
        counts.push_back(instances);previous(mode,count,type,indices,instances);
    }
};
struct NativeFormats {
    static inline PFNGLVERTEXARRAYATTRIBFORMATPROC previous{};
    static inline unsigned calls{};
    NativeFormats() { calls=0;previous=glad_glVertexArrayAttribFormat;glad_glVertexArrayAttribFormat=format; }
    ~NativeFormats() { glad_glVertexArrayAttribFormat=previous; }
    static void GLAD_API_PTR format(GLuint vao,GLuint index,GLint size,GLenum type,GLboolean normalized,GLuint offset) {
        ++calls;previous(vao,index,size,type,normalized,offset);
    }
};
Mesh triangle() {
    Mesh mesh(3);
    mesh.vertices<Vertex>()[0].set(Position{},Vec3{-.6F,-.5F,0});
    mesh.vertices<Vertex>()[1].set(Position{},Vec3{.6F,-.5F,0});
    mesh.vertices<Vertex>()[2].set(Position{},Vec3{0,.6F,0});
    for(auto& vertex:mesh.vertices<Vertex>())vertex.set(Color{},Vec4{.6F,.25F,.1F,1});
    for(auto& surface:mesh.vertices<Surface>()) {
        surface.set(Normal{},Vec3{0,0,1});surface.set(Emission{},.2F);
    }
    mesh.faces().push_back({0,1,2});return mesh;
}
}

TEST_CASE("Blueprint tickets produce real instanced draws and match individual shading", "[editor][opengl][batch]") {
    constexpr Extent2D extent{128,96};
    auto window=test::create_hidden_opengl_window(extent.width,extent.height,"mesh ticket batches");
    if(!window){std::cerr<<window.error().message<<'\n';std::exit(77);}
    auto access=window->make_current();REQUIRE(access);
    auto device=opengl::Device::create(*access);REQUIRE(device);
    auto program=BlueprintMeshRenderer::create_program(*device);REQUIRE(program);
    auto cpu=triangle();
    auto gpu=opengl::upload_mesh(*device,cpu);REQUIRE(gpu);
    auto baseline_mesh=opengl::upload_mesh(*device,cpu);REQUIRE(baseline_mesh);
    BlueprintMeshRenderer renderer{std::move(*gpu),*program};
    auto neutral=shader_program();REQUIRE(neutral);
    auto baseline=render::compile_program(*device,*neutral);REQUIRE(baseline);
    auto target=opengl::RenderTarget::create(*device,{.color=gfx::ImageFormat::rgba8,.depth=true},extent);REQUIRE(target);
    gfx::Camera camera;camera.set_position({0,0,5}).look_at({0,0,0}).set_orthographic({.vertical_height=3});
    auto view=render::RenderView::create(camera,extent);REQUIRE(view);
    std::vector<MeshDraw> tickets;
    for(unsigned i=0;i<3;++i) {
        MeshDraw ticket;
        ticket.transform=mesh_transform(ViewMode::scene,{.position={float(i)*1.2F-1.2F,0,0},
            .rotation={float(i)*8,float(i)*13,0},.scale=.75F});
        ticket.lighting.set(Eye{},Vec3{0,0,5});
        ticket.lighting.set(Light{},Vec4{float(i)-1,2,4,1});
        ticket.lighting.set(Brightness{},.12F+float(i)*.1F);
        tickets.push_back(ticket);
    }
    const auto draw=[&](bool batched) {
        auto frame=render::begin_frame(*device,*target,{.extent=extent,.color_encoding=render::ColorEncoding::linear,
            .clear_color=std::array<f32,4>{0,0,0,1},.clear_depth=1});REQUIRE(frame);
        if(batched) {REQUIRE(renderer.render(*frame,*view,tickets));}
        else {
            auto commands=frame->render_context();auto graphics=commands.graphics_state();
            REQUIRE(graphics.set(render::CullMode::none));REQUIRE(graphics.set(render::DepthState{true,true}));
            REQUIRE(graphics.set(render::BlendMode::disabled));REQUIRE(graphics.set(opengl::PolygonMode::fill));
            for(const auto& ticket:tickets) {
                REQUIRE(commands.run(*baseline,ticket.transform,ticket.lighting));REQUIRE(commands.view(*view));
                REQUIRE(commands.draw(*baseline_mesh));
            }
        }
        REQUIRE(frame->end());
        std::vector<u8> pixels(extent.width*extent.height*4);
        glGetTextureImage(target->color().native_handle(),0,GL_RGBA,GL_UNSIGNED_BYTE,static_cast<GLsizei>(pixels.size()),pixels.data());
        REQUIRE(glGetError()==GL_NO_ERROR);return pixels;
    };
    NativeDraws native;
    NativeFormats formats;
    auto instanced=draw(true);CHECK(NativeDraws::counts==std::vector<GLsizei>{3});
    CHECK(renderer.stats().instances==3);CHECK(renderer.stats().draw_calls==1);
    NativeDraws::counts.clear();auto individual=draw(false);
    CHECK(NativeDraws::counts==std::vector<GLsizei>{1,1,1});
    unsigned maximum_difference{},covered{};
    for(std::size_t i=0;i<individual.size();++i) {
        maximum_difference=std::max(maximum_difference,static_cast<unsigned>(std::abs(int(individual[i])-int(instanced[i]))));
        if(i%4==0 && instanced[i]>0)++covered;
    }
    CHECK(maximum_difference<=1);CHECK(covered>200);
    const auto configured = NativeFormats::calls;
    CHECK(draw(true) == instanced);
    CHECK(NativeFormats::calls == configured);
    auto prototype=tickets.front();tickets.assign(1024,prototype);
    NativeDraws::counts.clear();draw(true);
    CHECK(NativeDraws::counts==std::vector<GLsizei>{1024});CHECK(renderer.mesh().cached_vertex_input_count()==1);
    tickets.resize(4);tickets[1].wireframe=tickets[3].wireframe=true;
    NativeDraws::counts.clear();draw(true);
    CHECK(NativeDraws::counts==std::vector<GLsizei>{2,2});CHECK(renderer.stats().draw_calls==2);
    CHECK(renderer.mesh().cached_vertex_input_count()==1);
    CHECK(NativeFormats::calls == configured); // Growth and alternating solid/wire buffers only rebind storage.
    tickets.resize(1);NativeDraws::counts.clear();auto after_rebind=draw(true);
    CHECK(NativeDraws::counts==std::vector<GLsizei>{1});
    auto rebind_reference=draw(false);
    unsigned rebound_difference{};
    for(std::size_t i=0;i<after_rebind.size();++i)
        rebound_difference=std::max(rebound_difference,static_cast<unsigned>(
            std::abs(int(after_rebind[i])-int(rebind_reference[i]))));
    CHECK(rebound_difference<=1); // The cached VAO reads the current buffer, not the old/grown/wire batch.
    tickets.clear();NativeDraws::counts.clear();draw(true);
    CHECK(NativeDraws::counts.empty());CHECK(renderer.stats().instances==0);
    auto moved=std::move(renderer);renderer=std::move(moved);
    tickets.push_back(prototype);NativeDraws::counts.clear();draw(true);
    CHECK(NativeDraws::counts==std::vector<GLsizei>{1});
    CHECK(NativeFormats::calls == configured); // Moving the renderer preserves the cache too.

    // The engine-level typed instance buffer is also usable independently of
    // the example renderer; a bad shader contract fails before any draw.
    struct Unrelated : gfx::Semantic<Vec4> {};
    using WrongInstance=gfx::Record<Unrelated>;
    std::vector<WrongInstance> wrong(1);
    auto wrong_buffer=gfx::make_instance_buffer(*device,std::span<const WrongInstance>{wrong});REQUIRE(wrong_buffer);
    const auto storage=wrong_buffer->storage()->native_handle();
    wrong.resize(8);REQUIRE(wrong_buffer->update(*device,wrong));CHECK(wrong_buffer->storage()->native_handle()==storage);
    auto frame=render::begin_frame(*device,*target,{.extent=extent,.color_encoding=render::ColorEncoding::linear,
        .clear_color={},.clear_depth={}});REQUIRE(frame);
    auto commands=frame->render_context();REQUIRE(commands.run(*program));REQUIRE(commands.view(*view));
    NativeDraws::counts.clear();
    CHECK_FALSE(commands.draw(renderer.mesh(),*wrong_buffer));CHECK(NativeDraws::counts.empty());
    REQUIRE(frame->end());
}

TEST_CASE("Asteroid fleet submissions scale with blueprints rather than instance count", "[editor][opengl][batch][fleet]") {
    const auto path=std::filesystem::path(__FILE__).parent_path()/"../../examples/assets/asteroid_fleet.vscene";
    auto state=load_scene(path);REQUIRE(state);state->viewport.mode=ViewMode::scene;
    auto window=test::create_hidden_opengl_window(160,90,"fleet batching");if(!window)std::exit(77);
    auto access=window->make_current();REQUIRE(access);
    auto device=opengl::Device::create(*access);REQUIRE(device);
    auto runtime=Runtime::create(*device,*state);REQUIRE(runtime);
    REQUIRE(runtime->render(*device,*state,{160,90},false));
    const auto stats=runtime->stats();
    CHECK(stats.last_mesh_instances>500);
    CHECK(stats.last_mesh_renderer_calls==stats.resident_meshes);
    CHECK(stats.last_mesh_draw_calls==stats.last_mesh_renderer_calls);
    CHECK(stats.last_mesh_draw_calls<20);
    CHECK(stats.light_candidates == 0); // This scene has no sun: do not scan every mesh for every mesh.
    const auto evaluated = stats.sampled_instances;
    state->viewport.editor_camera.yaw += 12;
    REQUIRE(runtime->render_frame(*device,*state,{160,90}));
    CHECK(runtime->stats().sampled_instances == evaluated);
    std::cout<<"Fleet: "<<stats.last_mesh_instances<<" mesh instances / "<<stats.last_mesh_renderer_calls
             <<" blueprint submissions / "<<stats.last_mesh_draw_calls<<" native mesh draws\n";
}

TEST_CASE("Scene runtime submits mesh instances once per blueprint", "[editor][opengl][batch]") {
    auto window=test::create_hidden_opengl_window(128,96,"scene blueprint batches");
    if(!window)std::exit(77);
    auto access=window->make_current();REQUIRE(access);
    auto device=opengl::Device::create(*access);REQUIRE(device);
    content::vmesh::Document source;
    source.vertex_count=3;source.faces={{0,1,2}};
    source.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{-1,-1,0,1,-1,0,0,1,0}}};
    auto mesh=editor::EditableMesh::create(source);REQUIRE(mesh);
    State state{.document={.mesh=std::move(*mesh)}};
    state.viewport.mode=ViewMode::scene;sun_settings(state,2)->visible=false;
    for(unsigned i=0;i<31;++i)REQUIRE(instantiate(state,BlueprintId::mesh));
    auto runtime=Runtime::create(*device,state);REQUIRE(runtime);
    REQUIRE(runtime->render(*device,state,{128,96},false));
    CHECK(runtime->stats().resident_meshes==1);
    CHECK(runtime->stats().last_mesh_instances==32);
    CHECK(runtime->stats().last_mesh_renderer_calls==1);
    CHECK(runtime->stats().last_mesh_draw_calls==1);
    mesh_settings(state,1)->wireframe=true;
    REQUIRE(runtime->render(*device,state,{128,96},false));
    CHECK(runtime->stats().last_mesh_renderer_calls==1);
    CHECK(runtime->stats().last_mesh_draw_calls==2);
    mesh_settings(state,1)->visible=false;
    REQUIRE(runtime->render(*device,state,{128,96},false));
    CHECK(runtime->stats().last_mesh_instances==31);CHECK(runtime->stats().last_mesh_draw_calls==1);
}
