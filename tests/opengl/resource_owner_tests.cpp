#include <vng/resources_opengl/mesh_renderer.hpp>
#include <vng/opengl/resources.hpp>
#include <vng/opengl/framebuffer.hpp>
#include <vng/providers/mesh.hpp>
#include <vng/providers/image.hpp>
#include <vng/providers/program.hpp>
#include <vng/shader/shader.hpp>
#include <glad/gl.h>
#include <catch2/catch_test_macros.hpp>
#include "../support/glfw_opengl.hpp"
#include <cstdlib>
#include <iostream>
#include <optional>
#include <cmath>
#include <limits>

namespace {
namespace res=vng::resources;
namespace gl=vng::opengl;
namespace gfx=vng::gfx;
namespace render=vng::render;
struct Harness { vng::glfw_opengl::Window window; gl::Device device; };
Harness harness() {
    auto window=vng::test::create_hidden_opengl_window(64,64,"resource owner tests");
    if (!window) { std::cerr<<window.error().message<<'\n'; std::exit(77); }
    auto access=window->make_current(); REQUIRE(access);
    auto device=gl::Device::create(*access); REQUIRE(device);
    return {std::move(*window),std::move(*device)};
}
template<class T,class E> void ok(const std::expected<T,E>& value) {
    if (!value) { INFO(value.error().message); }
    REQUIRE(value);
}
render::SurfaceMesh triangle(float x=0) {
    render::SurfaceMesh mesh(3);
    const std::array positions{vng::Vec3{x-.7F,-.7F,0},vng::Vec3{x+.7F,-.7F,0},vng::Vec3{x,.7F,0}};
    for (std::size_t i=0;i<3;++i) {
        mesh.vertices()[i].set(gfx::Position{},positions[i]);
        mesh.vertices()[i].set(gfx::Color{}, {1,1,1,1});
    }
    mesh.faces().emplace_back(0,1,2); return mesh;
}
auto color(vng::u8 r,vng::u8 g,vng::u8 b) {
    gfx::ImageData pixels{.extent={1,1},.pixels={std::byte{r},std::byte{g},std::byte{b},std::byte{255}}};
    return vng::providers::texture(std::move(pixels),{.format=gfx::ImageFormat::rgba8,.generate_mipmaps=false});
}
struct NativeDraws {
    static inline PFNGLDRAWELEMENTSINSTANCEDPROC previous{};
    static inline std::vector<GLsizei> counts;
    NativeDraws() { counts.clear(); previous=glad_glDrawElementsInstanced; glad_glDrawElementsInstanced=draw; }
    ~NativeDraws() { glad_glDrawElementsInstanced=previous; }
    static void GLAD_API_PTR draw(GLenum mode,GLsizei count,GLenum type,const void* indices,GLsizei instances) {
        counts.push_back(instances); previous(mode,count,type,indices,instances);
    }
};
auto uniform_surface_shader() {
    using namespace vng;
    using VI=shader::VertexInputs<gfx::Position,gfx::TexCoord<>,gfx::Color>;
    using VO=shader::VertexOutputs<shader::ClipPosition,shader::smooth<gfx::TexCoord<>>,shader::smooth<gfx::Color>>;
    using FI=shader::FragmentInputs<shader::smooth<gfx::TexCoord<>>,shader::smooth<gfx::Color>>;
    auto vertex=shader::vertex<VI,VO>([](auto& s,dsl::Float4x4 model) {
        const auto world=model*dsl::vec4(s.input(gfx::Position{}),1.F);
        return s.output(dsl::field<shader::ClipPosition>(s.camera().project(world.xyz())),
            dsl::field<gfx::TexCoord<>>(s.input(gfx::TexCoord<>{})),dsl::field<gfx::Color>(s.input(gfx::Color{})));
    });ok(vertex);
    auto fragment=shader::fragment<FI,shader::FragmentOutputs<shader::Color<0>>>(
        [](auto& s,dsl::Float4 tint,dsl::Float emission) {
            const auto color=s.template sample_2d<0>(s.input(gfx::TexCoord<>{}))*tint*s.input(gfx::Color{});
            return s.output(dsl::field<shader::Color<0>>(dsl::vec4(color.xyz()*(emission+1.F),color.w())));
        });ok(fragment);
    return shader::link(std::move(*vertex),std::move(*fragment));
}
struct UnsupportedSurfaceField : gfx::Semantic<vng::Vec3> {};
template<bool Unsupported=false>
auto custom_surface_shader() {
    using namespace vng;
    using namespace render::surface;
    using VI=std::conditional_t<Unsupported,
        shader::VertexInputs<gfx::Position,ModelColumn<0>,ModelColumn<1>,ModelColumn<2>,ModelColumn<3>,Tint,Emission,UnsupportedSurfaceField>,
        shader::VertexInputs<gfx::Position,ModelColumn<0>,ModelColumn<1>,ModelColumn<2>,ModelColumn<3>,Tint,Emission>>;
    using VO=shader::VertexOutputs<shader::ClipPosition,shader::flat<Tint>,shader::flat<Emission>>;
    using FI=shader::FragmentInputs<shader::flat<Tint>,shader::flat<Emission>>;
    auto vertex=shader::vertex<VI,VO>([](auto& s) {
        auto position=s.input(gfx::Position{});
        if constexpr(Unsupported) position=position+s.input(UnsupportedSurfaceField{});
        const auto model=dsl::make<Mat4>(s.input(ModelColumn<0>{}),s.input(ModelColumn<1>{}),
            s.input(ModelColumn<2>{}),s.input(ModelColumn<3>{}));
        const auto world=model*dsl::vec4(position,1.F);
        return s.output(dsl::field<shader::ClipPosition>(s.camera().project(world.xyz())),
            dsl::field<Tint>(s.input(Tint{})),dsl::field<Emission>(s.input(Emission{})));
    });ok(vertex);
    auto fragment=shader::fragment<FI,shader::FragmentOutputs<shader::Color<0>>>([](auto& s) {
        const auto tint=s.input(Tint{});
        return s.output(dsl::field<shader::Color<0>>(dsl::vec4(tint.xyz()*(s.input(Emission{})+1.F),tint.w())));
    });ok(fragment);
    return shader::link(std::move(*vertex),std::move(*fragment));
}
}

TEST_CASE("provider builders outlive their recipes and build independent GPU resources","[resources][opengl]") {
    auto h=harness();
    auto pair=[&] {
        auto builder=render::mesh_renderer_builder(h.device);
        builder.mesh(vng::providers::mesh(triangle())).albedo(color(255,0,0));
        auto a=builder.build(); auto b=builder.build(); ok(a); ok(b);
        return std::pair{std::move(*a),std::move(*b)};
    }();
    CHECK(pair.first.albedo().native_handle()!=pair.second.albedo().native_handle());
    CHECK(pair.first.program().native_handle()!=pair.second.program().native_handle());
    auto source=pair.first.source_mesh().vertices()[0].get(gfx::Position{});
    auto result=pair.first.reload(h.device); ok(result);
    CHECK(result->refreshed.size()==3); CHECK(result->retained.empty());
    CHECK(pair.first.source_mesh().vertices()[0].get(gfx::Position{})==source);
    CHECK(pair.first.revision()==2); CHECK(pair.second.revision()==1);
}

TEST_CASE("ready replacements carry optional providers without reallocation or stale source resurrection","[resources][opengl]") {
    auto h=harness();
    auto builder=render::mesh_renderer_builder(h.device);
    builder.mesh(vng::providers::mesh(triangle()));
    auto renderer=builder.build(); ok(renderer);
    auto calls=std::make_shared<int>(0);
    auto provider=res::provider([calls,source=color(0,255,0)](gl::Device& device) {
        ++*calls; return source.provide(device);
    });
    auto ready=provider.provide(h.device); ok(ready);
    const auto handle=ready->native_handle();
    ok(renderer->replace_albedo(h.device,res::provided(std::move(*ready),provider)));
    CHECK(*calls==1); CHECK(renderer->albedo().native_handle()==handle);
    ok(renderer->reload_albedo(h.device)); CHECK(*calls==2);
    auto manual=color(0,0,255).provide(h.device); ok(manual);
    const auto manual_handle=manual->native_handle();
    ok(renderer->replace_albedo(h.device,std::move(*manual)));
    auto no_source=renderer->reload_albedo(h.device);
    REQUIRE_FALSE(no_source); CHECK(no_source.error().code==res::ErrorCode::no_provider);
    auto report=renderer->reload(h.device); ok(report);
    CHECK(report->retained==std::vector<std::string>{"albedo"});
    CHECK(renderer->albedo().native_handle()==manual_handle); CHECK(*calls==2);
}

TEST_CASE("a failed grouped update preserves every live resource and its recipes","[resources][opengl]") {
    auto h=harness(); auto builder=render::mesh_renderer_builder(h.device);
    builder.mesh(vng::providers::mesh(triangle())).albedo(color(255,0,0));
    auto renderer=builder.build(); ok(renderer);
    const auto revision=renderer->revision();
    const auto old_texture=renderer->albedo().native_handle();
    const auto old_position=renderer->source_mesh().vertices()[0].get(gfx::Position{});
    auto failed=res::provider([](gl::Device&)->res::Result<gl::Image2D> {
        return std::unexpected(res::Diagnostic{.message="deliberate provider failure"});
    });
    auto update=renderer->update(h.device);
    update.mesh(vng::providers::mesh(triangle(.2F))).albedo(failed);
    auto committed=update.commit(); REQUIRE_FALSE(committed);
    CHECK(renderer->revision()==revision); CHECK(renderer->albedo().native_handle()==old_texture);
    CHECK(renderer->source_mesh().vertices()[0].get(gfx::Position{})==old_position);
    ok(renderer->reload_albedo(h.device)); // original source, not the failed source
    auto obsolete=renderer->update(h.device);
    ok(renderer->reload_mesh(h.device));
    REQUIRE_FALSE(obsolete.commit());
}

TEST_CASE("shared products and source snapshots retain explicit ownership","[resources][opengl]") {
    auto h=harness(); auto cpu=triangle(); auto source=vng::providers::mesh(cpu);
    cpu.vertices()[0].set(gfx::Position{}, {100,100,100});
    auto builder=render::mesh_renderer_builder(h.device); builder.mesh(source);
    auto a=builder.build(); auto b=builder.build(); ok(a); ok(b);
    CHECK(a->source_mesh().vertices()[0].get(gfx::Position{}).x<0.0F);
    auto image=color(100,100,100).provide(h.device); ok(image);
    auto shared=res::share_resource(std::move(*image));
    ok(a->replace_albedo(h.device,shared)); ok(b->replace_albedo(h.device,shared));
    CHECK(a->albedo().native_handle()==b->albedo().native_handle());
    auto pending=a->update(h.device); pending.mesh(vng::providers::mesh(triangle(.2F)));
    auto frame=render::begin_frame(h.device,{.extent={64,64},.color_encoding=render::ColorEncoding::linear,
        .clear_color={},.clear_depth={}}); ok(frame);
    REQUIRE_FALSE(pending.commit()); CHECK(a->revision()==2);
    ok(frame->end()); ok(pending.commit()); CHECK(a->revision()==3);
}

TEST_CASE("textured mesh shader preserves HDR emission and rejects incompatible resources","[resources][opengl]") {
    auto h=harness(); auto builder=render::mesh_renderer_builder(h.device);
    builder.mesh(vng::providers::mesh(triangle())).albedo(color(255,0,0));
    auto renderer=builder.build(); ok(renderer);
    auto image=gl::Image2D::create(h.device,64,64,gl::ImageFormat::rgba16f); ok(image);
    auto depth=gl::Image2D::create(h.device,64,64,gl::ImageFormat::depth32f); ok(depth);
    auto target=gl::Framebuffer::create(h.device); ok(target);
    ok(target->attach_color(0,*image)); ok(target->attach_depth(*depth));
    auto frame=render::begin_frame(h.device,*target,{.extent={64,64},.color_encoding=render::ColorEncoding::linear,
        .clear_color=std::array<float,4>{0,0,0,1},.clear_depth=1}); ok(frame);
    gfx::Camera camera; camera.set_position({0,0,2}).look_at({0,0,0}).set_orthographic({.vertical_height=2});
    auto view=render::RenderView::create(camera,{64,64}); ok(view);
    auto parent=frame->render_context();auto graphics=parent.graphics_state();
    ok(graphics.set(render::DepthCompare::never));
    ok(graphics.set(render::FrontFace::clockwise));
    ok(graphics.set(render::BlendMode::additive));
    ok(graphics.set(gl::PolygonMode::line));
    ok(renderer->render(*frame,*view,render::SurfaceDraw{.emission=7}));
    CHECK(parent.active());
    auto pixel=target->read_rgba32f(0,32,32,1,1); ok(pixel);
    CHECK(pixel->front().r>7.9F); CHECK(pixel->front().g<.01F);
    ok(frame->end());
    auto integer=gl::Image2D::create(h.device,1,1,gl::ImageFormat::rg32ui); ok(integer);
    const auto previous=renderer->albedo().native_handle();
    REQUIRE_FALSE(renderer->replace_albedo(h.device,std::move(*integer)));
    CHECK(renderer->albedo().native_handle()==previous);
    auto grouped=renderer->update(h.device);
    grouped.mesh(vng::providers::mesh(triangle(.1F))).albedo(color(0,255,0));
    ok(grouped.commit()); CHECK(renderer->source_mesh().vertices()[0].get(gfx::Position{}).x>-.65F);
}

TEST_CASE("mesh adoption freezes aliased mutable vertices topology and provenance",
          "[resources][opengl]") {
    auto h=harness();
    auto authored=std::make_shared<render::SurfaceMesh>(triangle());
    authored->info().name="original";
    authored->explicit_edges().emplace().emplace_back(0,1);
    auto source=res::provider([authored]()->res::Result<render::MeshSource> {
        return render::MeshSource{authored};
    });
    auto builder=render::mesh_renderer_builder(h.device);
    auto renderer=builder.mesh(source).build(); ok(renderer);
    const auto original=renderer->source_mesh().vertices()[0].get(gfx::Position{});
    CHECK(&renderer->source_mesh()!=authored.get());

    authored->vertices()[0].set(gfx::Position{}, {-.4F,-.7F,0});
    authored->info().name="edited";
    authored->explicit_edges().reset();
    CHECK(renderer->source_mesh().vertices()[0].get(gfx::Position{})==original);
    CHECK(renderer->source_mesh().info().name=="original");
    REQUIRE(renderer->source_mesh().explicit_edges());
    CHECK(renderer->source_mesh().explicit_edges()->size()==1);
    ok(renderer->reload_mesh(h.device));
    CHECK(renderer->source_mesh().vertices()[0].get(gfx::Position{}).x==-.4F);
    CHECK(renderer->source_mesh().info().name=="edited");
    CHECK_FALSE(renderer->source_mesh().explicit_edges());

    const auto revision=renderer->revision();
    authored->faces()[0][0]=99;
    REQUIRE_FALSE(renderer->reload_mesh(h.device));
    CHECK(renderer->revision()==revision);
    CHECK(renderer->source_mesh().faces()[0][0]==0);
    ok(renderer->source_mesh().validate());

    auto ready=std::make_shared<render::SurfaceMesh>(triangle(.2F));
    const auto adopted=ready->vertices()[0].get(gfx::Position{});
    ok(renderer->replace_mesh(h.device,render::MeshSource{ready}));
    ready->vertices().clear(); ready->faces().clear();
    CHECK(renderer->source_mesh().vertex_count()==3);
    CHECK(renderer->source_mesh().face_count()==1);
    CHECK(renderer->source_mesh().vertices()[0].get(gfx::Position{})==adopted);
    REQUIRE_FALSE(renderer->reload_mesh(h.device));
}

TEST_CASE("surface draw arguments change without recompiling or packing a scratch matrix buffer",
          "[resources][opengl][arguments]") {
    auto h=harness();
    auto renderer=render::mesh_renderer_builder(h.device).mesh(vng::providers::mesh(triangle())).build();
    ok(renderer);
    const auto handle=renderer->program().native_handle();
    const auto* source=renderer->program().generated_source();
    REQUIRE(source);
    CHECK(source->matrix_buffer_bindings.empty());

    auto image=gl::Image2D::create(h.device,64,64,gl::ImageFormat::rgba16f); ok(image);
    auto target=gl::Framebuffer::create(h.device); ok(target);
    ok(target->attach_color(0,*image));
    gfx::Camera camera;
    camera.set_position({0,0,2}).look_at({0,0,0}).set_orthographic({.vertical_height=2});
    auto view=render::RenderView::create(camera,{64,64}); ok(view);
    auto draw_pixel=[&](render::SurfaceDraw ticket) {
        ticket.depth_test=false;
        ticket.depth_write=false;
        auto frame=render::begin_frame(h.device,*target,{
            .extent={64,64},.color_encoding=render::ColorEncoding::linear,
            .clear_color=std::array<float,4>{0,0,0,1},.clear_depth={}}); ok(frame);
        ok(renderer->render(*frame,*view,ticket));
        auto pixel=target->read_rgba32f(0,32,32,1,1); ok(pixel);
        ok(frame->end());
        CHECK(renderer->program().native_handle()==handle);
        return pixel->front();
    };
    render::SurfaceDraw ticket{.tint={0.25F,0.5F,1.0F,1.0F},.emission=3.0F};
    const auto bright=draw_pixel(ticket);
    CHECK(bright.r==1.0F); CHECK(bright.g==2.0F); CHECK(bright.b==4.0F);
    ticket.emission=0.0F;
    const auto dim=draw_pixel(ticket);
    CHECK(dim.r==0.25F); CHECK(dim.g==0.5F); CHECK(dim.b==1.0F);
    ticket.tint={1,0,0,1};
    const auto red=draw_pixel(ticket);
    CHECK(red.r==1.0F); CHECK(red.g==0.0F); CHECK(red.b==0.0F);
    ticket.transform[3].x=4.0F;
    const auto moved=draw_pixel(ticket);
    CHECK(moved.r==0.0F); CHECK(moved.g==0.0F); CHECK(moved.b==0.0F);
}

TEST_CASE("mesh rendering rejects albedo feedback before touching the active target",
          "[resources][opengl]") {
    auto h=harness(); auto builder=render::mesh_renderer_builder(h.device);
    auto renderer=builder.mesh(vng::providers::mesh(triangle())).build(); ok(renderer);
    auto image=gl::Image2D::create(h.device,64,64,gl::ImageFormat::rgba8); ok(image);
    auto shared=res::share_resource(std::move(*image));
    ok(renderer->replace_albedo(h.device,shared));
    auto target=gl::Framebuffer::create(h.device); ok(target);
    ok(target->attach_color(0,*shared));
    auto frame=render::begin_frame(h.device,*target,{
        .extent={64,64},.color_encoding=render::ColorEncoding::linear,
        .clear_color=std::array<float,4>{0,0,1,1},.clear_depth={}}); ok(frame);
    gfx::Camera camera;
    camera.set_position({0,0,2}).look_at({0,0,0}).set_orthographic({.vertical_height=2});
    auto view=render::RenderView::create(camera,{64,64}); ok(view);
    const auto revision=renderer->revision();
    auto feedback=renderer->render(*frame,*view,
        render::SurfaceDraw{.depth_test=false,.depth_write=false});
    REQUIRE_FALSE(feedback);
    CHECK(feedback.error().code==res::ErrorCode::invalid_argument);
    CHECK(feedback.error().message.find("attachment")!=std::string::npos);
    auto pixel=target->read_rgba8_pixels(0,32,32,1,1); ok(pixel);
    CHECK(pixel->front()==gl::Rgba8Pixel{0,0,255,255});
    CHECK(renderer->revision()==revision);
    ok(frame->end());
    ok(renderer->reload_albedo(h.device,color(255,0,0)));
    auto separate=render::begin_frame(h.device,*target,{
        .extent={64,64},.color_encoding=render::ColorEncoding::linear,
        .clear_color=std::array<float,4>{0,0,1,1},.clear_depth={}}); ok(separate);
    ok(renderer->render(*separate,*view,render::SurfaceDraw{.depth_test=false,.depth_write=false}));
    auto rendered=target->read_rgba8_pixels(0,32,32,1,1); ok(rendered);
    CHECK(rendered->front()==gl::Rgba8Pixel{255,0,0,255});
}

TEST_CASE("provided shared albedo adopts one snapshot and retains its recipe independently",
          "[resources][opengl]") {
    auto h=harness(); auto builder=render::mesh_renderer_builder(h.device);
    builder.mesh(vng::providers::mesh(triangle()));
    auto first=builder.build(); auto second=builder.build(); ok(first); ok(second);
    auto calls=std::make_shared<int>(0);
    auto recipe=res::share_provider(res::provider(
        [calls,source=color(0,255,0)](gl::Device& device)->res::Result<gl::SharedImage> {
            ++*calls;
            auto image=source.provide(device);
            if (!image) return std::unexpected(res::to_diagnostic(std::move(image.error())));
            return res::share_resource(std::move(*image));
        }));
    auto ready=recipe.provide(h.device); ok(ready);
    const auto shared_handle=(*ready)->native_handle();
    ok(first->replace_albedo(h.device,res::provided(*ready,recipe)));
    auto pending=second->update(h.device);
    pending.albedo(res::provided(std::move(*ready),recipe));
    ok(pending.commit());
    CHECK(*calls==1);
    CHECK(first->albedo().native_handle()==shared_handle);
    CHECK(second->albedo().native_handle()==shared_handle);
    ok(first->reload_albedo(h.device));
    CHECK(*calls==2);
    CHECK(first->albedo().native_handle()!=shared_handle);
    CHECK(second->albedo().native_handle()==shared_handle);
    ok(second->reload_albedo(h.device));
    CHECK(*calls==3);
    CHECK(second->albedo().native_handle()!=first->albedo().native_handle());
}

TEST_CASE("mesh update detects a provider changing its owner and cannot reenter the same commit",
          "[resources][opengl]") {
    auto h=harness(); auto builder=render::mesh_renderer_builder(h.device);
    builder.mesh(vng::providers::mesh(triangle())).albedo(color(255,0,0));
    auto renderer=builder.build(); ok(renderer);
    const auto revision=renderer->revision();
    const auto old_image=renderer->albedo().native_handle();
    auto nested_source=vng::providers::mesh(triangle(-.2F)).provide(); ok(nested_source);
    auto outer=renderer->update(h.device);
    outer.mesh(vng::providers::mesh(triangle(.2F)));
    outer.albedo(res::provider([&](gl::Device& device) {
        REQUIRE_FALSE(outer.commit());
        ok(renderer->replace_mesh(device,*nested_source));
        return color(0,255,0).provide(device);
    }));
    auto stale=outer.commit(); REQUIRE_FALSE(stale);
    CHECK(renderer->revision()==revision+1);
    CHECK(renderer->source_mesh().vertices()[0].get(gfx::Position{}).x<-.8F);
    CHECK(renderer->albedo().native_handle()==old_image);
    REQUIRE_FALSE(renderer->reload_mesh(h.device));
    ok(renderer->reload_albedo(h.device));

    auto one_attempt=renderer->update(h.device);
    one_attempt.albedo(res::provider([&](gl::Device& device) {
        REQUIRE_FALSE(one_attempt.commit());
        return color(0,0,255).provide(device);
    }));
    ok(one_attempt.commit());
    REQUIRE_FALSE(one_attempt.commit());
}

TEST_CASE("mesh update rechecks frame safety after arbitrary provider work",
          "[resources][opengl]") {
    auto h=harness(); auto builder=render::mesh_renderer_builder(h.device);
    auto renderer=builder.mesh(vng::providers::mesh(triangle())).build(); ok(renderer);
    const auto revision=renderer->revision();
    const auto old_image=renderer->albedo().native_handle();
    std::optional<gl::Frame> borrowed;
    auto opens_frame=res::provider([&](gl::Device& device) {
        auto frame=render::begin_frame(device,{
            .extent={64,64},.color_encoding=render::ColorEncoding::linear,
            .clear_color={},.clear_depth={}}); ok(frame);
        borrowed.emplace(std::move(*frame));
        return color(0,255,0).provide(device);
    });
    REQUIRE_FALSE(renderer->reload_albedo(h.device,opens_frame));
    REQUIRE(borrowed); REQUIRE(borrowed->active());
    CHECK(renderer->revision()==revision);
    CHECK(renderer->albedo().native_handle()==old_image);
    ok(borrowed->end()); borrowed.reset();
    ok(renderer->reload_albedo(h.device));
    builder.albedo(opens_frame);
    REQUIRE_FALSE(builder.build());
    REQUIRE(borrowed); REQUIRE(borrowed->active());
    ok(borrowed->end());
}

TEST_CASE("mesh update retains its executing provider and freezes candidate recipes",
          "[resources][opengl]") {
    auto h=harness(); auto builder=render::mesh_renderer_builder(h.device);
    builder.mesh(vng::providers::mesh(triangle())).albedo(color(255,0,0));
    auto renderer=builder.build(); ok(renderer);
    auto calls=std::make_shared<int>(0);
    auto marker=std::make_shared<int>(42);
    std::weak_ptr<int> lifetime=marker;
    auto pending=renderer->update(h.device);
    pending.albedo(res::provider(
        [marker=std::move(marker),calls,&pending](gl::Device& device) {
            ++*calls;
            pending.albedo(color(0,0,255));
            pending.mesh(vng::providers::mesh(triangle(.4F)));
            // The setter above dropped the pending update's reference to
            // this callable. Its captured evidence must still be alive.
            CHECK(*marker==42);
            return color(0,255,0).provide(device);
        }));
    ok(pending.commit());
    CHECK(*calls==1);
    CHECK_FALSE(lifetime.expired());
    CHECK(renderer->source_mesh().vertices()[0].get(gfx::Position{}).x==-.7F);
    REQUIRE_FALSE(pending.commit());
    ok(renderer->reload_albedo(h.device));
    CHECK(*calls==2);
    CHECK_FALSE(lifetime.expired());
    CHECK(renderer->source_mesh().vertices()[0].get(gfx::Position{}).x==-.7F);

    auto target=gl::Framebuffer::create(h.device); ok(target);
    ok(target->attach_color(0,renderer->albedo()));
    auto pixel=target->read_rgba8_pixels(0,0,0,1,1); ok(pixel);
    CHECK(pixel->front()==gl::Rgba8Pixel{0,255,0,255});
}

TEST_CASE("surface tickets become compatible instanced runs with per-ticket equivalent pixels",
          "[resources][opengl][batch]") {
    using namespace vng;
    auto h=harness();
    auto renderer=render::mesh_renderer_builder(h.device).mesh(providers::mesh(triangle())).build();ok(renderer);
    auto ir=uniform_surface_shader();ok(ir);
    auto reference=render::compile_program(h.device,*ir);ok(reference);
    auto geometry=gl::upload_mesh(h.device,triangle());ok(geometry);
    auto image=gl::Image2D::create(h.device,64,64,gl::ImageFormat::rgba16f);ok(image);
    auto depth=gl::Image2D::create(h.device,64,64,gl::ImageFormat::depth32f);ok(depth);
    auto target=gl::Framebuffer::create(h.device);ok(target);
    ok(target->attach_color(0,*image));ok(target->attach_depth(*depth));
    gfx::Camera camera;camera.set_position({0,0,2}).look_at({0,0,0}).set_orthographic({.vertical_height=2});
    auto view=render::RenderView::create(camera,{64,64});ok(view);
    std::vector<render::SurfaceDraw> tickets(3);
    for(unsigned i=0;i<3;++i) {
        auto& ticket=tickets[i];const float angle=float(i)*.4F,scale=.24F+float(i)*.06F;
        ticket.transform[0]={std::cos(angle)*scale,std::sin(angle)*scale,0,0};
        ticket.transform[1]={-std::sin(angle)*scale,std::cos(angle)*scale,0,0};
        ticket.transform[3]={float(i)*.6F-.6F,0,0,1};
        ticket.tint={.25F+float(i)*.25F,.5F,1.F-float(i)*.25F,1};ticket.emission=float(i);
    }
    const auto draw=[&](bool batch) {
        auto frame=render::begin_frame(h.device,*target,{.extent={64,64},.color_encoding=render::ColorEncoding::linear,
            .clear_color=std::array<float,4>{0,0,0,1},.clear_depth=1});ok(frame);
        if(batch)ok(renderer->render(*frame,*view,tickets));
        else {
            auto commands=frame->render_context();auto graphics=commands.graphics_state();
            ok(graphics.set(render::DepthCompare::less));ok(graphics.set(render::FrontFace::counter_clockwise));
            ok(graphics.set(render::BlendMode::disabled));ok(graphics.set(gl::PolygonMode::fill));
            glBindTextureUnit(0,renderer->albedo().native_handle());glBindSampler(0,0);
            for(const auto& ticket:tickets) {
                ok(commands.run(*reference,ticket.transform,ticket.tint,ticket.emission));ok(commands.view(*view));
                ok(graphics.set(render::DepthTest{ticket.depth_test}));ok(graphics.set(render::DepthWrite{ticket.depth_write}));
                ok(graphics.set(ticket.cull));ok(commands.draw(*geometry));
            }
        }
        auto pixels=target->read_rgba32f(0,0,0,64,64);ok(pixels);ok(frame->end());return *pixels;
    };
    const auto equivalent=[&](const auto& a,const auto& b) {
        REQUIRE(a.size()==b.size());
        float maximum{};unsigned covered{};
        for(std::size_t i=0;i<a.size();++i) {
            maximum=std::max({maximum,std::abs(a[i].r-b[i].r),std::abs(a[i].g-b[i].g),
                std::abs(a[i].b-b[i].b),std::abs(a[i].a-b[i].a)});
            if(a[i].r>0)++covered;
        }
        CHECK(maximum<=.002F);CHECK(covered>40);
    };
    NativeDraws native;
    const auto batched=draw(true);CHECK(NativeDraws::counts==std::vector<GLsizei>{3});
    CHECK(renderer->stats().instances==3);CHECK(renderer->stats().draw_calls==1);
    NativeDraws::counts.clear();const auto individual=draw(false);
    CHECK(NativeDraws::counts==std::vector<GLsizei>{1,1,1});equivalent(batched,individual);

    // No regrouping across incompatible state: A/A/B/B/C/A must remain in
    // submission order, including when depth is disabled and meshes overlap.
    tickets.assign(6,render::SurfaceDraw{.depth_test=false,.depth_write=false,.cull=render::CullMode::none});
    for(unsigned i=0;i<tickets.size();++i)tickets[i].tint={float(i)*.125F,1.F-float(i)*.125F,.25F,1};
    tickets[2].depth_write=tickets[3].depth_write=true;
    tickets[4].depth_test=true;tickets[4].cull=render::CullMode::back;
    NativeDraws::counts.clear();const auto ordered=draw(true);
    CHECK(NativeDraws::counts==std::vector<GLsizei>{2,2,1,1});
    CHECK(renderer->stats().instances==6);CHECK(renderer->stats().draw_calls==4);
    const auto ordered_reference=draw(false);equivalent(ordered,ordered_reference);
    CHECK(ordered[32*64+32].r==tickets.back().tint.x);

    tickets.assign(1024,tickets.front());NativeDraws::counts.clear();draw(true);
    CHECK(NativeDraws::counts==std::vector<GLsizei>{1024});
    CHECK(renderer->stats().instances==1024);CHECK(renderer->stats().draw_calls==1);
    tickets.resize(1);NativeDraws::counts.clear();draw(true);
    CHECK(NativeDraws::counts==std::vector<GLsizei>{1});
    tickets.clear();NativeDraws::counts.clear();draw(true);
    CHECK(NativeDraws::counts.empty());CHECK(renderer->stats().instances==0);CHECK(renderer->stats().draw_calls==0);

    // A malformed later ticket is rejected before even the first run draws.
    tickets.resize(2);tickets.back().emission=std::numeric_limits<float>::infinity();
    auto frame=render::begin_frame(h.device,*target,{.extent={64,64},.color_encoding=render::ColorEncoding::linear,
        .clear_color=std::array<float,4>{0,0,1,1},.clear_depth=1});ok(frame);
    NativeDraws::counts.clear();CHECK_FALSE(renderer->render(*frame,*view,tickets));CHECK(NativeDraws::counts.empty());
    auto untouched=target->read_rgba32f(0,32,32,1,1);ok(untouched);
    CHECK(untouched->front().b==1.F);CHECK(untouched->front().r==0.F);ok(frame->end());
}

TEST_CASE("surface program providers validate instancing contracts before atomic replacement",
          "[resources][opengl][batch][providers]") {
    using namespace vng;
    auto h=harness();
    auto renderer=render::mesh_renderer_builder(h.device).mesh(providers::mesh(triangle())).build();ok(renderer);
    const auto original=renderer->program().native_handle();
    const auto revision=renderer->revision();
    auto legacy=uniform_surface_shader();ok(legacy);
    auto uniform_only=res::provider([ir=std::move(*legacy)](gl::Device& device) {
        return render::compile_program(device,ir.untyped());
    });
    auto update=renderer->update(h.device);
    update.mesh(providers::mesh(triangle(.3F))).program(uniform_only);
    const auto rejected=update.commit();REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.find("instance")!=std::string::npos);
    CHECK(renderer->revision()==revision);CHECK(renderer->program().native_handle()==original);
    CHECK(renderer->source_mesh().vertices()[0].get(gfx::Position{}).x==-.7F);
    auto wrong=custom_surface_shader<true>();ok(wrong);
    CHECK_FALSE(renderer->reload_program(h.device,providers::program(std::move(*wrong))));
    CHECK(renderer->revision()==revision);CHECK(renderer->program().native_handle()==original);
    auto missing_vertex=shader::vertex<shader::VertexInputs<gfx::Position>,
        shader::VertexOutputs<shader::ClipPosition>>([](auto& s) {
            return s.output(dsl::field<shader::ClipPosition>(s.camera().project(s.input(gfx::Position{}))));
        });ok(missing_vertex);
    auto missing_fragment=shader::fragment<shader::FragmentInputs<>,shader::FragmentOutputs<shader::Color<0>>>(
        [](auto& s) {return s.output(dsl::field<shader::Color<0>>(s.constant(Vec4{1,1,1,1})));});ok(missing_fragment);
    auto missing=shader::link(std::move(*missing_vertex),std::move(*missing_fragment));ok(missing);
    CHECK_FALSE(renderer->reload_program(h.device,providers::program(std::move(*missing))));
    CHECK(renderer->revision()==revision);CHECK(renderer->program().native_handle()==original);
    // Failed provider never replaces the retained working recipe.
    ok(renderer->reload_program(h.device));CHECK(renderer->revision()==revision+1);
    auto custom=custom_surface_shader();ok(custom);
    auto provider=providers::program(std::move(*custom));
    ok(renderer->reload_program(h.device,provider));
    const auto replaced=renderer->program().native_handle();
    ok(renderer->reload_program(h.device));CHECK(renderer->program().native_handle()!=replaced);
    auto builder=render::mesh_renderer_builder(h.device);
    builder.mesh(providers::mesh(triangle())).program(provider);auto second=builder.build();ok(second);

    auto image=gl::Image2D::create(h.device,64,64,gl::ImageFormat::rgba16f);ok(image);
    auto target=gl::Framebuffer::create(h.device);ok(target);ok(target->attach_color(0,*image));
    gfx::Camera camera;camera.set_position({0,0,2}).look_at({0,0,0}).set_orthographic({.vertical_height=2});
    auto view=render::RenderView::create(camera,{64,64});ok(view);
    auto frame=render::begin_frame(h.device,*target,{.extent={64,64},.color_encoding=render::ColorEncoding::linear,
        .clear_color=std::array<float,4>{0,0,0,1},.clear_depth={}});ok(frame);
    const std::array tickets{render::SurfaceDraw{.tint={.25F,.5F,1,1},.emission=3,.depth_test=false},
        render::SurfaceDraw{.tint={1,.5F,.25F,1},.emission=1,.depth_test=false}};
    NativeDraws native;ok(second->render(*frame,*view,tickets));CHECK(NativeDraws::counts==std::vector<GLsizei>{2});
    auto pixel=target->read_rgba32f(0,32,32,1,1);ok(pixel);
    CHECK(pixel->front().r==2.F);CHECK(pixel->front().g==1.F);CHECK(pixel->front().b==.5F);
    ok(frame->end());
}
