#include <vng/opengl/bloom.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/render/frame.hpp>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

#include "../support/glfw_opengl.hpp"

namespace {

vng::glfw_opengl::Window bloom_window()
{
    auto window = vng::test::create_hidden_opengl_window(64, 64, "vng bloom test");
    if (!window) {
        std::cerr << "OpenGL bloom test skipped: " << window.error().message << '\n';
        std::exit(77);
    }
    return std::move(*window);
}

vng::render::FrameDesc description(vng::Extent2D extent)
{
    return {.extent = extent, .color_encoding = vng::render::ColorEncoding::linear,
        .clear_color = std::nullopt, .clear_depth = std::nullopt};
}

void write_bright_patch(const vng::opengl::CurrentContextAccess& access,
    const vng::opengl::Image2D& image)
{
    using Upload = void (*)(vng::u32, vng::i32, vng::i32, vng::i32,
        vng::i32, vng::i32, vng::u32, vng::u32, const void*);
    const auto upload = reinterpret_cast<Upload>(access.resolve("glTextureSubImage2D"));
    REQUIRE(upload);
    std::array<std::array<float, 4>, 16> pixels{};
    for (auto& pixel : pixels) pixel = {8.0F, 4.0F, 2.0F, 0.375F};
    upload(image.native_handle(), 0, 30, 30, 4, 4, 0x1908, 0x1406, pixels.data());
}

} // namespace

TEST_CASE("Bloom creates an HDR halo, controls tone mapping, and preserves alpha",
          "[opengl][bloom]")
{
    auto window = bloom_window();
    auto access = window.make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto scene = vng::opengl::Image2D::create(*device, 64, 64, vng::opengl::ImageFormat::rgba16f);
    REQUIRE(scene);
    REQUIRE(scene->clear_rgba32f({0, 0, 0, 0.375F}));
    write_bright_patch(*access, *scene);
    auto image = vng::opengl::Image2D::create(*device, 64, 64, vng::opengl::ImageFormat::rgba32f);
    REQUIRE(image);
    auto target = vng::opengl::Framebuffer::create(*device);
    REQUIRE(target);
    REQUIRE(target->attach_color(0, *image));

    auto bloom = [&] {
        auto builder = vng::render::bloom_builder(*device);
        builder.levels(5);
        return builder.build({64, 64});
    }();
    REQUIRE(bloom);
    CHECK(bloom->levels() == 5);
    auto frame = vng::render::begin_frame(*device, *target, description({64, 64}));
    REQUIRE(frame);

    REQUIRE(bloom->apply(*frame, *scene, {.strength = 0}));
    auto unlit = target->read_rgba32f(0, 0, 0, 64, 64);
    REQUIRE(unlit);
    const auto center = 32 * 64 + 32;
    const auto outside = 32 * 64 + 36;
    CHECK(std::abs((*unlit)[center].r - 8.0F / 9.0F) < 0.0001F);
    CHECK(std::abs((*unlit)[center].g - 4.0F / 5.0F) < 0.0001F);
    CHECK(std::abs((*unlit)[center].b - 2.0F / 3.0F) < 0.0001F);
    CHECK((*unlit)[outside].r == 0.0F);

    REQUIRE(bloom->apply(*frame, *scene, {.strength = 1}));
    auto glowing = target->read_rgba32f(0, 0, 0, 64, 64);
    REQUIRE(glowing);
    CHECK((*glowing)[outside].r > 0.005F);
    CHECK((*glowing)[outside].r > (*glowing)[outside].g);
    CHECK((*glowing)[outside].g > (*glowing)[outside].b);
    CHECK((*glowing)[center].r > (*unlit)[center].r);
    CHECK((*glowing)[0].r < (*glowing)[outside].r);
    for (const auto& pixel : *glowing) {
        REQUIRE(std::isfinite(pixel.r));
        REQUIRE(std::isfinite(pixel.g));
        REQUIRE(std::isfinite(pixel.b));
        REQUIRE(pixel.a == 0.375F);
    }

    REQUIRE(bloom->apply(*frame, *scene, {.threshold = 100, .strength = 1}));
    auto below_threshold = target->read_rgba32f(0, 0, 0, 64, 64);
    REQUIRE(below_threshold);
    CHECK((*below_threshold)[outside].r == 0.0F);
    CHECK(std::abs((*below_threshold)[center].r - (*unlit)[center].r) < 0.0001F);
    REQUIRE(bloom->apply(*frame, *scene, {.strength = 0, .exposure = 2}));
    auto exposed = target->read_rgba32f(0, 32, 32, 1, 1);
    REQUIRE(exposed);
    CHECK(std::abs((*exposed)[0].r - 16.0F / 17.0F) < 0.0001F);
    REQUIRE(frame->end());
    REQUIRE(bloom->reload(*device));
    auto reloaded_frame = vng::render::begin_frame(*device, *target, description({64, 64}));
    REQUIRE(reloaded_frame);
    REQUIRE(bloom->apply(*reloaded_frame, *scene, {.strength = 1}));
    auto reloaded = target->read_rgba32f(0, 36, 32, 1, 1);
    REQUIRE(reloaded);
    CHECK(std::abs((*reloaded)[0].r - (*glowing)[outside].r) < 0.0001F);
    REQUIRE(reloaded_frame->end());
    CHECK(device->take_lifecycle_diagnostics().empty());
}

TEST_CASE("Bloom rebuilds at frame boundaries and retains resources on rejected updates",
          "[opengl][bloom][resources]")
{
    auto window = bloom_window();
    auto access = window.make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto builder = vng::render::bloom_builder(*device);
    builder.levels(8);
    auto bloom = builder.build({64, 64});
    REQUIRE(bloom);
    auto other = builder.build({16, 16});
    REQUIRE(other);
    CHECK(bloom->levels() == 6);
    CHECK(other->levels() == 4);
    REQUIRE_FALSE(builder.levels(0).build({64, 64}));
    REQUIRE_FALSE(builder.levels(9).build({64, 64}));
    REQUIRE_FALSE(builder.levels(4).build({0, 64}));
    REQUIRE_FALSE(bloom->resize(*device, {0, 64}));
    REQUIRE_FALSE(bloom->resize(*device, {std::numeric_limits<vng::u32>::max(), 64}));
    CHECK(bloom->extent() == vng::Extent2D{64, 64});
    CHECK(bloom->levels() == 6);

    auto scene = vng::opengl::Image2D::create(*device, 64, 64, vng::opengl::ImageFormat::rgba16f);
    REQUIRE(scene);
    REQUIRE(scene->clear_rgba32f({2, 1, 0, 1}));
    auto target = vng::opengl::Framebuffer::create(*device);
    REQUIRE(target);
    REQUIRE(target->attach_color(0, *scene));
    auto feedback = vng::render::begin_frame(*device, *target, description({64, 64}));
    REQUIRE(feedback);
    REQUIRE_FALSE(bloom->apply(*feedback, *scene));
    REQUIRE_FALSE(bloom->resize(*device, {16, 16}));
    REQUIRE_FALSE(bloom->reload(*device));
    CHECK(bloom->extent() == vng::Extent2D{64, 64});
    REQUIRE(feedback->end());
    auto output = vng::render::begin_frame(*device, description({64, 64}));
    REQUIRE(output);
    REQUIRE(bloom->apply(*output, *scene));
    REQUIRE_FALSE(bloom->apply(*output, *scene,
        {.threshold = std::numeric_limits<float>::quiet_NaN()}));
    REQUIRE_FALSE(bloom->apply(*output, *scene, {.strength = -1}));
    REQUIRE_FALSE(bloom->apply(*output, *scene,
        {.exposure = std::numeric_limits<float>::infinity()}));
    REQUIRE(output->end());
    REQUIRE_FALSE(bloom->apply(*output, *scene));

    auto integer_image = vng::opengl::Image2D::create(
        *device, 64, 64, vng::opengl::ImageFormat::rgba32ui);
    REQUIRE(integer_image);
    auto integer_target = vng::opengl::Framebuffer::create(*device);
    REQUIRE(integer_target);
    REQUIRE(integer_target->attach_color(0, *integer_image));
    auto integer_frame = vng::render::begin_frame(*device, *integer_target, description({64, 64}));
    REQUIRE(integer_frame);
    REQUIRE_FALSE(bloom->apply(*integer_frame, *scene));
    REQUIRE(integer_frame->end());

    REQUIRE(bloom->resize(*device, {3, 7}));
    CHECK(bloom->extent() == vng::Extent2D{3, 7});
    CHECK(bloom->levels() == 2);
    CHECK(other->extent() == vng::Extent2D{16, 16});
    REQUIRE(bloom->reload(*device));
    REQUIRE(bloom->resize(*device, {1, 1}));
    CHECK(bloom->levels() == 1);
    auto tiny = vng::opengl::Image2D::create(*device, 1, 1, vng::opengl::ImageFormat::rgba16f);
    REQUIRE(tiny);
    REQUIRE(tiny->clear_rgba32f({2, 1, 0, 1}));
    auto tiny_frame = vng::render::begin_frame(*device, description({1, 1}));
    REQUIRE(tiny_frame);
    REQUIRE(bloom->apply(*tiny_frame, *tiny));
    REQUIRE(tiny_frame->end());
    auto moved = std::move(*bloom);
    CHECK(bloom->extent().empty());
    REQUIRE_FALSE(bloom->reload(*device));
    REQUIRE(moved.reload(*device));
}

TEST_CASE("Bloom restores caller raster, target, texture and sampler state",
          "[opengl][bloom][state]")
{
    auto window = bloom_window();
    auto access = window.make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto bloom = vng::render::bloom_builder(*device).build({64, 64});
    REQUIRE(bloom);
    auto source = vng::opengl::Image2D::create(*device, 64, 64, vng::opengl::ImageFormat::rgba16f);
    REQUIRE(source);
    REQUIRE(source->clear_rgba32f({4, 2, 1, 0.5F}));
    auto target_image = vng::opengl::Image2D::create(*device, 64, 64, vng::opengl::ImageFormat::rgba32f);
    REQUIRE(target_image);
    auto target = vng::opengl::Framebuffer::create(*device);
    REQUIRE(target);
    REQUIRE(target->attach_color(0, *target_image));
    auto auxiliary = vng::opengl::Image2D::create(*device, 64, 64, vng::opengl::ImageFormat::rgba32f);
    REQUIRE(auxiliary);
    REQUIRE(auxiliary->clear_rgba32f({7, 6, 5, 4}));
    REQUIRE(target->attach_color(1, *auxiliary));

    using Get = void (*)(vng::u32, vng::i32*);
    using GetIndexed = void (*)(vng::u32, vng::u32, vng::i32*);
    using GetBoolIndexed = void (*)(vng::u32, vng::u32, vng::u8*);
    using IsEnabled = vng::u8 (*)(vng::u32);
    using CreateSamplers = void (*)(vng::i32, vng::u32*);
    using DeleteSamplers = void (*)(vng::i32, const vng::u32*);
    using BindSampler = void (*)(vng::u32, vng::u32);
    using ActiveTexture = void (*)(vng::u32);
    const auto get = reinterpret_cast<Get>(access->resolve("glGetIntegerv"));
    const auto get_indexed = reinterpret_cast<GetIndexed>(access->resolve("glGetIntegeri_v"));
    const auto get_bool = reinterpret_cast<GetBoolIndexed>(access->resolve("glGetBooleani_v"));
    const auto enabled = reinterpret_cast<IsEnabled>(access->resolve("glIsEnabled"));
    const auto create_samplers = reinterpret_cast<CreateSamplers>(access->resolve("glCreateSamplers"));
    const auto delete_samplers = reinterpret_cast<DeleteSamplers>(access->resolve("glDeleteSamplers"));
    const auto bind_sampler = reinterpret_cast<BindSampler>(access->resolve("glBindSampler"));
    const auto active_texture = reinterpret_cast<ActiveTexture>(access->resolve("glActiveTexture"));
    REQUIRE(get); REQUIRE(get_indexed); REQUIRE(get_bool); REQUIRE(enabled);
    REQUIRE(create_samplers); REQUIRE(delete_samplers); REQUIRE(bind_sampler); REQUIRE(active_texture);
    std::array<vng::u32, 2> samplers{};
    create_samplers(2, samplers.data());
    for (vng::u32 unit = 0; unit < 2; ++unit) {
        REQUIRE(source->bind_to_unit(unit));
        bind_sampler(unit, samplers[unit]);
    }
    active_texture(0x84C0 + 3);
    auto frame = vng::render::begin_frame(*device, *target, description({64, 64}));
    REQUIRE(frame);
    auto old_commands = frame->commands();
    REQUIRE(device->set_depth_state({true, true, vng::opengl::DepthCompare::never}));
    REQUIRE(device->set_blend_enabled(0, true));
    REQUIRE(device->set_color_write_mask(0, {false, true, false, false}));
    REQUIRE(device->set_scissor_enabled(true));
    REQUIRE(device->set_rasterizer_discard_enabled(true));
    REQUIRE(device->viewport(3, 4, 5, 6));
    REQUIRE(bloom->apply(*frame, *source));
    CHECK(old_commands.active());
    CHECK(frame->active());
    std::array<vng::i32, 4> viewport{};
    get(0x0BA2, viewport.data());
    CHECK(viewport == std::array<vng::i32, 4>{3, 4, 5, 6});
    CHECK(enabled(0x0B71) == 1); // GL_DEPTH_TEST
    CHECK(enabled(0x0BE2) == 1); // GL_BLEND
    CHECK(enabled(0x0C11) == 1); // GL_SCISSOR_TEST
    CHECK(enabled(0x8C89) == 1); // GL_RASTERIZER_DISCARD
    std::array<vng::u8, 4> mask{};
    get_bool(0x0C23, 0, mask.data());
    CHECK(mask == std::array<vng::u8, 4>{0, 1, 0, 0});
    vng::i32 draw{}, read{}, active{};
    get(0x8CA6, &draw); get(0x8CAA, &read); get(0x84E0, &active);
    CHECK(draw == static_cast<vng::i32>(target->native_handle()));
    CHECK(read == static_cast<vng::i32>(target->native_handle()));
    CHECK(active == 0x84C0 + 3);
    for (vng::u32 unit = 0; unit < 2; ++unit) {
        vng::i32 texture{}, sampler{};
        get_indexed(0x8069, unit, &texture); get_indexed(0x8919, unit, &sampler);
        CHECK(texture == static_cast<vng::i32>(source->native_handle()));
        CHECK(sampler == static_cast<vng::i32>(samplers[unit]));
        bind_sampler(unit, 0);
    }
    delete_samplers(2, samplers.data());
    auto pixel = target->read_rgba32f(0, 32, 32, 1, 1);
    REQUIRE(pixel);
    CHECK((*pixel)[0].r > 0.8F);
    CHECK((*pixel)[0].g > 0.6F);
    CHECK((*pixel)[0].b > 0.5F);
    CHECK((*pixel)[0].a == 0.5F);
    auto retained = target->read_rgba32f(1, 32, 32, 1, 1);
    REQUIRE(retained);
    CHECK((*retained)[0].r == 7.0F);
    CHECK((*retained)[0].g == 6.0F);
    CHECK((*retained)[0].b == 5.0F);
    CHECK((*retained)[0].a == 4.0F);
    REQUIRE(frame->end());
}

TEST_CASE("parent commands remain usable after bloom's explicit native state scope",
          "[opengl][bloom][composition]")
{
    using namespace vng;
    auto window = bloom_window();
    auto access = window.make_current(); REQUIRE(access);
    auto device = opengl::Device::create(*access); REQUIRE(device);
    auto bloom = render::bloom_builder(*device).build({64,64}); REQUIRE(bloom);
    auto source = opengl::Image2D::create(*device,64,64,opengl::ImageFormat::rgba16f); REQUIRE(source);
    REQUIRE(source->clear_rgba32f({2,0,0,1}));
    auto image = opengl::Image2D::create(*device,64,64,opengl::ImageFormat::rgba32f); REQUIRE(image);
    auto target = opengl::Framebuffer::create(*device); REQUIRE(target);
    REQUIRE(target->attach_color(0,*image));
    struct Position : gfx::Semantic<Vec2> {};
    using Vertex = gfx::Record<Position>;
    auto vertex = shader::vertex<shader::VertexInputs<Position>,shader::VertexOutputs<shader::ClipPosition>>([](auto& s) {
        return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(s.input(Position{}),0.0F,1.0F)));
    });
    auto fragment = shader::fragment<shader::FragmentInputs<>,shader::FragmentOutputs<shader::Color<0>>>([](auto& s) {
        return s.output(dsl::field<shader::Color<0>>(s.constant(Vec4{0,1,0,1})));
    });
    REQUIRE(vertex); REQUIRE(fragment);
    auto ir = shader::link(std::move(*vertex),std::move(*fragment)); REQUIRE(ir);
    auto program = render::compile_program(*device,*ir); REQUIRE(program);
    gfx::Mesh<Vertex> mesh(3);
    mesh.vertices()[0].set(Position{}, {-0.8F,-0.8F});
    mesh.vertices()[1].set(Position{}, {0.8F,-0.8F});
    mesh.vertices()[2].set(Position{}, {0,0.8F});
    mesh.add_face(0,1,2);
    auto gpu = opengl::upload_mesh(*device,mesh); REQUIRE(gpu);
    auto frame = render::begin_frame(*device,*target,description({64,64})); REQUIRE(frame);
    auto parent = frame->render_context();
    auto graphics = parent.graphics_state();
    REQUIRE(parent.run(*program));
    REQUIRE(graphics.set(render::DepthState{true,true,render::DepthCompare::never}));
    REQUIRE(graphics.set(opengl::PolygonMode::line));
    REQUIRE(bloom->apply(*frame,*source,{.strength=0}));
    REQUIRE(parent.active());
    // The scope restored raw state, but invalidated managed bindings. Reselect
    // explicitly through the existing handle; no new context is required.
    REQUIRE_FALSE(parent.draw(*gpu));
    REQUIRE(parent.run(*program));
    REQUIRE(graphics.set(render::DepthState{false,false}));
    REQUIRE(graphics.set(render::CullMode::none));
    REQUIRE(graphics.set(render::BlendMode::disabled));
    REQUIRE(graphics.set(opengl::PolygonMode::fill));
    REQUIRE(parent.draw(*gpu));
    auto pixels = target->read_rgba32f(0,0,0,64,64); REQUIRE(pixels);
    CHECK((*pixels)[32*64+32].r==0);
    CHECK((*pixels)[32*64+32].g==1);
    CHECK((*pixels)[0].r>0.6F);
    CHECK((*pixels)[0].g==0);
    REQUIRE(frame->end());
    CHECK_FALSE(parent.active());
}

TEST_CASE("Deleting a program cannot leave a stale binding for bloom state restoration",
          "[opengl][bloom][lifecycle]")
{
    auto window = bloom_window();
    auto access = window.make_current();
    REQUIRE(access);
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);
    auto bloom = vng::render::bloom_builder(*device).build({64, 64});
    REQUIRE(bloom);
    auto source = vng::opengl::Image2D::create(*device, 64, 64, vng::opengl::ImageFormat::rgba16f);
    REQUIRE(source);
    REQUIRE(source->clear_rgba32f({2, 1, 0, 1}));
    auto vertex = vng::shader::vertex<vng::shader::VertexInputs<>,
        vng::shader::VertexOutputs<vng::shader::ClipPosition>>("lifetime_vertex", [](auto& s) {
        return s.output(vng::dsl::field<vng::shader::ClipPosition>(s.constant(vng::Vec4{0, 0, 0, 1})));
    });
    REQUIRE(vertex);
    auto fragment = vng::shader::fragment<vng::shader::FragmentInputs<>,
        vng::shader::FragmentOutputs<vng::shader::Color<0>>>("lifetime_fragment", [](auto& s) {
        return s.output(vng::dsl::field<vng::shader::Color<0>>(s.constant(vng::Vec4{1, 1, 1, 1})));
    });
    REQUIRE(fragment);
    auto ir = vng::shader::link(std::move(*vertex), std::move(*fragment));
    REQUIRE(ir);
    auto retained = vng::render::compile_program(*device, *ir);
    REQUIRE(retained);
    REQUIRE(retained->bind());
    auto discarded = vng::render::compile_program(*device, *ir);
    REQUIRE(discarded);
    const auto deleted_handle = discarded->native_handle();
    vng::u32 expected_current{};

    SECTION("explicit destroy unbinds the deleted program") {
        REQUIRE(discarded->bind());
        REQUIRE(discarded->destroy());
    }
    SECTION("RAII destruction unbinds the deleted program") {
        auto scoped = std::move(*discarded);
        REQUIRE(scoped.bind());
    }
    SECTION("move assignment releases the old bound program") {
        REQUIRE(discarded->bind());
        auto replacement = vng::render::compile_program(*device, *ir);
        REQUIRE(replacement);
        *discarded = std::move(*replacement);
    }
    SECTION("deleting an unbound program preserves the unrelated binding") {
        REQUIRE(discarded->destroy());
        expected_current = retained->native_handle();
    }

    using Get = void (*)(vng::u32, vng::i32*);
    using IsProgram = vng::u8 (*)(vng::u32);
    const auto get = reinterpret_cast<Get>(access->resolve("glGetIntegerv"));
    const auto is_program = reinterpret_cast<IsProgram>(access->resolve("glIsProgram"));
    REQUIRE(get);
    REQUIRE(is_program);
    vng::i32 current{};
    get(0x8B8D, &current); // GL_CURRENT_PROGRAM
    CHECK(current == static_cast<vng::i32>(expected_current));
    CHECK(is_program(deleted_handle) == 0);

    auto frame = vng::render::begin_frame(*device, description({64, 64}));
    REQUIRE(frame);
    auto result = bloom->apply(*frame, *source);
    if (!result) INFO(result.error().message);
    REQUIRE(result);
    get(0x8B8D, &current);
    CHECK(current == static_cast<vng::i32>(expected_current));
    REQUIRE(frame->end());
    CHECK(device->take_lifecycle_diagnostics().empty());
}
