#include "../../examples/editor/mesh_overlay.hpp"
#include "../../examples/editor/runtime.hpp"
#include "../../examples/support/presentation.hpp"
#include "../support/glfw_opengl.hpp"
#include <vng/opengl/ui_renderer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glad/gl.h>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {
using namespace vng;
using namespace editor_example;
const auto assets=std::filesystem::path{__FILE__}.parent_path()/"../../examples/assets";
text::Font font() {auto f=text::Font::load(assets/"fonts/DejaVuSans.ttf");REQUIRE(f);return *f;}
struct Context {
    glfw_opengl::Window window;
    opengl::Device device;
    static Context create() {
        auto window=test::create_hidden_opengl_window(800,600,"mesh overlay regression");
        if(!window) {std::cerr<<window.error().message<<'\n';std::exit(77);}
        auto token=window->make_current();REQUIRE(token);
        auto device=opengl::Device::create(*token);REQUIRE(device);
        return {std::move(*window),std::move(*device)};
    }
};
}
TEST_CASE("Dense spaceship overlays stay resident across frames and camera navigation", "[editor][opengl][mesh]") {
    auto context=Context::create();auto& device=context.device;
    auto ship=editor::EditableMesh::load(assets/"spaceship.vmesh");REQUIRE(ship);
    State state{.document={.mesh=std::move(*ship)}};state.viewport.mode=ViewMode::mesh;
    ui::Screen screen{ui::dark_theme(font())};MeshTools tools{screen.column(),screen.column()};tools.sync(state);
    auto overlay=MeshOverlay::create(device);REQUIRE(overlay);
    constexpr Extent2D extent{800,600};
    auto target=opengl::RenderTarget::create(device,{.color=gfx::ImageFormat::srgb8_alpha8,.depth=true},extent);REQUIRE(target);
    gfx::Camera camera;camera.set_position({0,-8,10}).look_at({0,0,0});
    const auto render=[&] {
        auto frame=render::begin_frame(device,*target,{.extent=extent,.color_encoding=render::ColorEncoding::srgb,.clear_color=std::array<f32,4>{.3F,.3F,.3F,1},.clear_depth=1});REQUIRE(frame);
        REQUIRE(overlay->render(*frame,state,tools,{0,0,800,600},{800,600},extent,camera));
        REQUIRE(frame->end());
    };
    render();CHECK(overlay->stats().position_uploads==1);CHECK(overlay->stats().index_uploads==2);
    CHECK(overlay->stats().topology_uploads==1);CHECK(overlay->stats().selection_uploads==1);
    const auto start=std::chrono::steady_clock::now();
    for(int i=0;i<20;++i) {
        camera.set_position({static_cast<f32>(i)*.02F,-8,10}).look_at({0,0,0});
        ++state.viewport.sequence;tools.sync(state);render();
        CHECK(overlay->stats().position_uploads==0);CHECK(overlay->stats().index_uploads==0);CHECK(overlay->stats().draws<=8);
    }
    const auto submitted=std::chrono::steady_clock::now();
    REQUIRE(device.finish());
    std::cout<<"Resident spaceship overlay, camera moving: "<<std::chrono::duration<double,std::milli>(submitted-start).count()/20<<" ms CPU/frame; "
        <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/20<<" ms/frame including GPU completion\n";
    tools.select(7,true);render();CHECK(overlay->stats().position_uploads==0);CHECK(overlay->stats().index_uploads==1);
    CHECK(overlay->stats().topology_uploads==0);CHECK(overlay->stats().selection_uploads==1);
    auto p=state.document.mesh.position(7);p.x+=.1F;REQUIRE(state.document.mesh.set_position(7,p));++state.document.revision;tools.sync(state);
    render();CHECK(overlay->stats().position_uploads>0);CHECK(overlay->stats().index_uploads==0);
    tools.mode(MeshSelectMode::face);tools.select_all(state.document.mesh);
    REQUIRE(tools.hide_selected()==state.document.mesh.document().faces.size());
    render();CHECK(overlay->stats().position_uploads==0);CHECK(overlay->stats().draws==0);
    REQUIRE(tools.reveal_hidden()==state.document.mesh.document().faces.size());
    tools.mode(MeshSelectMode::vertex);
    // Compose real underside/top shading and the overlay for visual inspection.
    auto runtime=Runtime::create(device,state);REQUIRE(runtime);
    auto ui_renderer=opengl::UiRenderer::create(device);REQUIRE(ui_renderer);
    char folder[]="/tmp/vng-mesh-visual-XXXXXX";REQUIRE(::mkdtemp(folder));
    for(const auto below:{false,true}) {
        camera.set_position({5,below?-8.F:8.F,10}).look_at({0,0,0});
        auto pixels=runtime->render(device,RenderRequest{state,camera,extent,0,false});REQUIRE(pixels);
        const std::string name=below?"below":"above";
        REQUIRE(example::write_rgba8_png(std::filesystem::path{folder}/(name+"-raw.png"),extent,
            {reinterpret_cast<const u8*>(pixels->pixels.data()),pixels->pixels.size()}));
        auto frame=render::begin_frame(device,*target,{.extent=extent,.color_encoding=render::ColorEncoding::srgb,.clear_depth=1});REQUIRE(frame);
        ui::DrawList list{{800,600},extent,{ui::ImageDraw{{0,0,800,600},{0,0,800,600},std::make_shared<const gfx::ImageData>(std::move(*pixels)),{},1}}};
        REQUIRE(ui_renderer->render(*frame,list));REQUIRE(overlay->render(*frame,state,tools,{0,0,800,600},{800,600},extent,camera));REQUIRE(frame->end());
        std::vector<u8> rgba(800*600*4);glGetTextureImage(target->color().native_handle(),0,GL_RGBA,GL_UNSIGNED_BYTE,static_cast<GLsizei>(rgba.size()),rgba.data());
        for(u32 y=0;y<300;++y) for(u32 x=0;x<800*4;++x) std::swap(rgba[y*800*4+x],rgba[(599-y)*800*4+x]);
        REQUIRE(example::write_rgba8_png(std::filesystem::path{folder}/(name+"-overlay.png"),extent,rgba));
    }
    std::cout<<"Mesh visual evidence: "<<folder<<'\n';
}
TEST_CASE("Mesh overlay depth hides back vertices while X-ray is explicit", "[editor][opengl][mesh]") {
    auto context=Context::create();auto& device=context.device;
    content::vmesh::Document d;d.vertex_count=4;
    d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{0,0,-1,-20,-20,0,20,-20,0,0,20,0}}};d.faces={{1,2,3}};
    auto mesh=editor::EditableMesh::create(std::move(d));REQUIRE(mesh);
    State state{.document={.mesh=std::move(*mesh)}};state.viewport.mode=ViewMode::mesh;
    ui::Screen screen{ui::dark_theme(font())};MeshTools tools{screen.column(),screen.column()};tools.sync(state);
    auto overlay=MeshOverlay::create(device);REQUIRE(overlay);
    constexpr Extent2D extent{100,100};auto target=opengl::RenderTarget::create(device,{.color=gfx::ImageFormat::rgba8,.depth=true},extent);REQUIRE(target);
    gfx::Camera camera;camera.set_position({0,0,5}).look_at({0,0,0});
    CHECK_FALSE(tools.pick(state,{.5F,.5F},extent,camera,{0,0,100,100}));
    for(bool xray:{false,true}) {
        tools.xray(xray);
        auto frame=render::begin_frame(device,*target,{.extent=extent,.color_encoding=render::ColorEncoding::linear,.clear_color=std::array<f32,4>{1,1,1,1},.clear_depth=1});REQUIRE(frame);
        REQUIRE(overlay->render(*frame,state,tools,{0,0,100,100},{100,100},extent,camera));REQUIRE(frame->end());
        std::vector<u8> rgba(100*100*4);glGetTextureImage(target->color().native_handle(),0,GL_RGBA,GL_UNSIGNED_BYTE,static_cast<GLsizei>(rgba.size()),rgba.data());
        CHECK((rgba[(50*100+50)*4+2]<100)==xray);
    }
    CHECK(tools.pick(state,{.5F,.5F},extent,camera,{0,0,100,100})==0);
    // H removes the occluder from the local depth pass, leaving the loose
    // vertex behind it visible without enabling X-ray.
    tools.mode(MeshSelectMode::face);tools.select(0,false);REQUIRE(tools.hide_selected()==1);
    tools.mode(MeshSelectMode::vertex);tools.xray(false);
    CHECK(tools.pick(state,{.5F,.5F},extent,camera,{0,0,100,100})==0);
    auto frame=render::begin_frame(device,*target,{.extent=extent,.color_encoding=render::ColorEncoding::linear,.clear_color=std::array<f32,4>{1,1,1,1},.clear_depth=1});REQUIRE(frame);
    REQUIRE(overlay->render(*frame,state,tools,{0,0,100,100},{100,100},extent,camera));REQUIRE(frame->end());
    CHECK(overlay->stats().position_uploads==0);
    std::vector<u8> rgba(100*100*4);glGetTextureImage(target->color().native_handle(),0,GL_RGBA,GL_UNSIGNED_BYTE,static_cast<GLsizei>(rgba.size()),rgba.data());
    CHECK(rgba[(50*100+50)*4+2]<100);
}
TEST_CASE("Opening RMB with two selected vertices preserves every overlay pixel", "[editor][opengl][mesh][tool-options]") {
    auto context=Context::create();auto& device=context.device;
    content::vmesh::Document d;d.vertex_count=3;
    d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},
        std::vector<f32>{-1,-1,0,1,-1,0,0,1,0}}};d.faces={{0,1,2}};
    auto mesh=editor::EditableMesh::create(std::move(d));REQUIRE(mesh);
    State state{.document={.mesh=std::move(*mesh)}};state.viewport.mode=ViewMode::mesh;
    ui::Screen screen{ui::dark_theme(font())};
    MeshTools tools{screen.column(),screen.column()};tools.sync(state);
    tools.mode(MeshSelectMode::vertex);tools.select(0,false);tools.select(1,true);
    REQUIRE(tools.selected().size()==2);
    auto overlay=MeshOverlay::create(device);REQUIRE(overlay);
    constexpr Extent2D extent{256,256};
    auto target=opengl::RenderTarget::create(device,{.color=gfx::ImageFormat::rgba8,.depth=true},extent);REQUIRE(target);
    gfx::Camera camera;camera.set_position({0,0,5}).look_at({0,0,0});
    const auto capture=[&] {
        auto frame=render::begin_frame(device,*target,{.extent=extent,.color_encoding=render::ColorEncoding::linear,
            .clear_color=std::array<f32,4>{.3F,.3F,.3F,1},.clear_depth=1});REQUIRE(frame);
        REQUIRE(overlay->render(*frame,state,tools,{0,0,256,256},{256,256},extent,camera));
        REQUIRE(frame->end());
        REQUIRE(overlay->stats().draws>=5); // depth, edges and vertex/selection marker passes
        std::vector<u8> pixels(256*256*4);
        glGetTextureImage(target->color().native_handle(),0,GL_RGBA,GL_UNSIGNED_BYTE,
            static_cast<GLsizei>(pixels.size()),pixels.data());
        return pixels;
    };
    const auto before=capture();
    tools.open({128,128},{256,256},{ui::Rect{0,0,256,256}});
    REQUIRE(tools.menu_open());
    const auto during=capture();
    CHECK(during==before);
    CHECK(overlay->stats().position_uploads==0);
    CHECK(overlay->stats().index_uploads==0);
    tools.close();
    CHECK(capture()==before);
    CHECK(tools.selected().size()==2);
}
TEST_CASE("Vertex mode draws legible constant-screen-size dots and orange selection", "[editor][opengl][mesh]") {
    auto context=Context::create();auto& device=context.device;
    content::vmesh::Document d;d.vertex_count=1;
    d.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>{0,0,0}}};
    auto mesh=editor::EditableMesh::create(std::move(d));REQUIRE(mesh);
    State state{.document={.mesh=std::move(*mesh)}};state.viewport.mode=ViewMode::mesh;
    ui::Screen screen{ui::dark_theme(font())};MeshTools tools{screen.column(),screen.column()};tools.sync(state);
    auto overlay=MeshOverlay::create(device);REQUIRE(overlay);
    for(u32 scale:{1U,2U}) {
        const Extent2D extent{100*scale,100*scale};
        auto target=opengl::RenderTarget::create(device,{.color=gfx::ImageFormat::rgba8,.depth=true},extent);REQUIRE(target);
        for(float distance:{5.F,20.F}) for(bool selected:{false,true}) {
            tools.select_all(state.document.mesh,!selected);
            gfx::Camera camera;camera.set_position({0,0,distance}).look_at({0,0,0});
            auto frame=render::begin_frame(device,*target,{.extent=extent,.color_encoding=render::ColorEncoding::linear,.clear_color=std::array<f32,4>{0,0,0,1},.clear_depth=1});REQUIRE(frame);
            REQUIRE(overlay->render(*frame,state,tools,{0,0,100,100},{100,100},extent,camera));REQUIRE(frame->end());
            std::vector<u8> rgba(extent.width*extent.height*4);
            glGetTextureImage(target->color().native_handle(),0,GL_RGBA,GL_UNSIGNED_BYTE,static_cast<GLsizei>(rgba.size()),rgba.data());
            const auto channel=[&](u32 x,u32 y,u32 c){return rgba[(y*extent.width+x)*4+c];};
            const auto center=50*scale;
            if(selected) {
                CHECK(channel(center,center,0)>240);CHECK(channel(center,center,2)<20);
                CHECK(channel(center+2*scale,center,0)>240);
            } else {
                CHECK(channel(center,center,0)<10);
                CHECK(channel(center+2*scale,center,0)>90); // contrasting rim, not a 2px speck
            }
            CHECK(channel(center+5*scale,center,0)==0); // stays compact when zooming / on HiDPI
        }
    }
}
TEST_CASE("Surface mode leaves the rendered image untouched and submits no overlay work", "[editor][opengl][mesh]") {
    auto context=Context::create();auto& device=context.device;
    auto mesh=editor::EditableMesh::load(assets/"colored_cube.vmesh");REQUIRE(mesh);
    State state{.document={.mesh=std::move(*mesh)}};state.viewport.mode=ViewMode::mesh;
    ui::Screen screen{ui::dark_theme(font())};MeshTools tools{screen.column(),screen.column()};tools.sync(state);
    auto overlay=MeshOverlay::create(device);REQUIRE(overlay);
    constexpr Extent2D extent{128,128};
    auto target=opengl::RenderTarget::create(device,{.color=gfx::ImageFormat::rgba8,.depth=true},extent);REQUIRE(target);
    gfx::Camera camera;camera.set_position({0,0,5}).look_at({0,0,0});
    const auto pixels=[&] {
        std::vector<u8> result(extent.width*extent.height*4);
        glGetTextureImage(target->color().native_handle(),0,GL_RGBA,GL_UNSIGNED_BYTE,
            static_cast<GLsizei>(result.size()),result.data());
        return result;
    };
    for(auto edit_mode:{MeshSelectMode::vertex,MeshSelectMode::edge,MeshSelectMode::face})
    for(auto preview_mode:{MeshSelectMode::surface,MeshSelectMode::whole}) {
        auto frame=render::begin_frame(device,*target,{.extent=extent,.color_encoding=render::ColorEncoding::linear,
            .clear_color=std::array<f32,4>{.2F,.4F,.6F,1},.clear_depth=.75F});REQUIRE(frame);
        const auto untouched=pixels();
        tools.mode(preview_mode);
        // Test existing backend state too: Surface must not override a renderer's
        // own wireframe choice or clear/rebuild depth merely to hide the overlay.
        glPolygonMode(GL_FRONT_AND_BACK,GL_LINE);
        REQUIRE(overlay->render(*frame,state,tools,{0,0,128,128},{128,128},extent,camera));
        CHECK(pixels()==untouched);
        CHECK(overlay->stats().draws==0);
        CHECK(overlay->stats().position_uploads==0);
        CHECK(overlay->stats().index_uploads==0);
        GLint polygon[2]{};glGetIntegerv(GL_POLYGON_MODE,polygon);
        CHECK(polygon[0]==GL_LINE);
        GLfloat depth{};glReadPixels(64,64,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depth);
        CHECK(depth>=.74999F);CHECK(depth<=.75001F);
        glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
        tools.mode(edit_mode);tools.select_all(state.document.mesh);
        REQUIRE(overlay->render(*frame,state,tools,{0,0,128,128},{128,128},extent,camera));
        CHECK(overlay->stats().draws>0);
        CHECK(pixels()!=untouched);
        REQUIRE(frame->end());
    }
}
