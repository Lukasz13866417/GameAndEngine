#include <vng/gfx/gfx.hpp>
#include <vng/gfx/geometry.hpp>
#include <vng/opengl/buffer.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/skinned_mesh_renderer.hpp>
#include <vng/render/frame.hpp>
#include <vng/render/view.hpp>
#include <vng/render/skinned_mesh_renderer.hpp>
#include <vng/rig/rig.hpp>
#include <vng/providers/skin.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../support/glfw_opengl.hpp"

namespace {

constexpr vng::Extent2D extent{128, 128};
using Pixel = std::array<std::uint8_t, 4>;

struct Harness final {
    vng::glfw_opengl::Window window;
    vng::opengl::CurrentContextAccess access;
    vng::opengl::Device device;
};

Harness create_harness()
{
    auto window = vng::test::create_hidden_opengl_window(
        extent.width, extent.height, "vng skinned renderer test");
    if (!window) {
        std::cerr << "OpenGL rigging test skipped: " << window.error().message << '\n';
        std::exit(77);
    }
    auto access = window->make_current();
    if (!access) {
        std::cerr << "OpenGL rigging test skipped: " << access.error().message << '\n';
        std::exit(77);
    }
    auto device = vng::opengl::Device::create(*access);
    if (!device) INFO(device.error().message);
    REQUIRE(device);
    return {std::move(*window), *access, std::move(*device)};
}

vng::opengl::Frame begin_clear(Harness& harness)
{
    auto frame = vng::render::begin_frame(
        harness.device,
        vng::render::FrameDesc{
            .extent = extent,
            .color_encoding = vng::render::ColorEncoding::linear,
            .clear_color = std::array<float, 4>{0, 0, 0, 1},
            .clear_depth = 1.0F,
        });
    if (!frame) INFO(frame.error().message);
    REQUIRE(frame);
    return std::move(*frame);
}

std::vector<Pixel> read_pixels(const Harness& harness)
{
    using ReadPixels = void (*)(std::int32_t, std::int32_t,
        std::int32_t, std::int32_t, std::uint32_t, std::uint32_t, void*);
    const auto read = reinterpret_cast<ReadPixels>(
        harness.access.resolve("glReadPixels"));
    REQUIRE(read != nullptr);
    std::vector<Pixel> pixels(extent.width * extent.height);
    read(0, 0, static_cast<std::int32_t>(extent.width),
         static_cast<std::int32_t>(extent.height),
         0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, pixels.data());
    return pixels;
}

bool red(const Pixel& pixel)
{
    return pixel[0] > 240 && pixel[1] < 8 && pixel[2] < 8;
}

bool black(const Pixel& pixel)
{
    return pixel[0] < 8 && pixel[1] < 8 && pixel[2] < 8;
}

const Pixel& at_ndc(const std::vector<Pixel>& pixels, float x, float y)
{
    const auto column = static_cast<vng::u32>((x + 1.0F) * 0.5F * extent.width);
    const auto row = static_cast<vng::u32>((y + 1.0F) * 0.5F * extent.height);
    REQUIRE(column < extent.width);
    REQUIRE(row < extent.height);
    return pixels[row * extent.width + column];
}

template<class T, class E>
void require_ok(const std::expected<T, E>& result)
{
    if (!result) INFO(result.error().message);
    REQUIRE(result);
}

vng::render::RenderView view()
{
    // At Z=0 this camera maps world X/Y exactly to NDC X/Y. Pixel checks then
    // independently describe the expected deformation without shader helpers.
    vng::gfx::Camera camera;
    camera.set_position({0, 0, 2}).look_at({0, 0, 0}).set_orthographic({
        .vertical_height = 2.0F, .near_plane = 0.1F, .far_plane = 10.0F,
    });
    auto result = vng::render::RenderView::create(camera, extent);
    require_ok(result);
    return *result;
}

using Vertex = vng::gfx::Record<vng::gfx::Position, vng::gfx::Color>;
using Mesh = vng::gfx::Mesh<Vertex>;
constexpr std::array triangle_positions{
    vng::Vec3{-0.12F, -0.12F, 0},
    vng::Vec3{0.12F, -0.12F, 0},
    vng::Vec3{0, 0.16F, 0},
};

Mesh triangle()
{
    Mesh mesh{3};
    for (std::size_t i = 0; i < triangle_positions.size(); ++i) {
        mesh.vertices()[i].set(vng::gfx::Position{}, triangle_positions[i]);
        mesh.vertices()[i].set(vng::gfx::Color{}, {1, 0, 0, 1});
    }
    mesh.add_face(0, 1, 2);
    return mesh;
}

struct SimpleArmature final {
    vng::rig::Armature armature;
    vng::rig::BoneId root;
    vng::rig::BoneId child;
};

SimpleArmature armature()
{
    vng::rig::ArmatureBuilder builder;
    const auto root = builder.add_bone("root");
    const auto child = builder.add_bone("child", root);
    auto built = builder.build();
    require_ok(built);
    return {std::move(*built), root, child};
}

vng::rig::SkinBinding<Mesh> weighted_triangle(const SimpleArmature& skeleton)
{
    auto binding = vng::rig::bind(triangle(), skeleton.armature);
    require_ok(binding);
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
        require_ok(binding->set_weights(vertex, {{skeleton.root, 1.0F}}));
    }
    return std::move(*binding);
}

} // namespace

TEST_CASE("skinned renderer keeps the bind pose and follows an animated root",
          "[opengl][rig]")
{
    auto harness = create_harness();
    auto skeleton = armature();
    auto binding = weighted_triangle(skeleton);
    auto renderer = vng::render::make_skinned_mesh_renderer(harness.device, binding);
    require_ok(renderer);
    STATIC_CHECK(vng::render::RendererFor<
        vng::opengl::SkinnedMeshRenderer, vng::opengl::Frame>);

    auto pose = skeleton.armature.rest_pose();
    auto frame = begin_clear(harness);
    require_ok(renderer->render(frame, view(), vng::render::SkinnedDraw{.pose = pose}));
    const auto rest_pixels = read_pixels(harness);
    CHECK(red(at_ndc(rest_pixels, 0, 0)));
    CHECK(black(at_ndc(rest_pixels, 0.5F, 0)));
    CHECK(std::ranges::count_if(rest_pixels, red) > 100);
    CHECK(renderer->stats().draw_calls == 1);
    CHECK(renderer->stats().triangles == 1);
    require_ok(frame.end());

    require_ok(pose.set_local(skeleton.root,
        vng::rig::Transform{.translation = {0.5F, 0, 0}}));
    auto animated_frame = begin_clear(harness);
    auto parent = animated_frame.render_context();
    auto graphics = parent.graphics_state();
    require_ok(graphics.set(vng::render::DepthCompare::never));
    require_ok(graphics.set(vng::render::FrontFace::clockwise));
    require_ok(graphics.set(vng::render::BlendMode::additive));
    require_ok(graphics.set(vng::opengl::PolygonMode::line));
    require_ok(renderer->render(animated_frame, view(),
        vng::render::SkinnedDraw{.pose = pose}));
    CHECK(parent.active());
    const auto animated_pixels = read_pixels(harness);
    CHECK(black(at_ndc(animated_pixels, 0, 0)));
    CHECK(red(at_ndc(animated_pixels, 0.5F, 0)));
    CHECK(renderer->stats().palette_uploads == 1);
    // An exact half-width translation is 32 pixels, so rasterization should
    // preserve the complete triangle's coverage count.
    CHECK(std::ranges::count_if(animated_pixels, red)
        == std::ranges::count_if(rest_pixels, red));
}

TEST_CASE("skin providers preserve animated poses and committed rendering across reload failure",
          "[opengl][rig][resources]")
{
    auto harness = create_harness();
    auto skeleton = armature();
    auto binding = weighted_triangle(skeleton);
    auto calls = std::make_shared<int>(0);
    auto recipe = vng::resources::share_provider(vng::resources::provider(
        [calls, skin = vng::providers::skin(binding)] {
            ++*calls;
            return skin.provide();
        }));
    auto build = [&] {
        auto builder = vng::render::skinned_mesh_renderer_builder(harness.device);
        builder.skin(recipe, vng::render::GeometryFields{}).options({.lighting = false});
        auto first = builder.build();
        require_ok(first);
        auto second = builder.build();
        require_ok(second);
        return first;
    };
    auto renderer = build();
    CHECK(*calls == 2);
    auto pose = skeleton.armature.rest_pose();
    require_ok(pose.set_local(skeleton.root,
        vng::rig::Transform{.translation = {0.5F, 0, 0}}));
    const auto revision = pose.revision();
    auto frame = begin_clear(harness);
    require_ok(renderer->render(frame, view(), vng::render::SkinnedDraw{.pose = pose}));
    const auto pixels = read_pixels(harness);
    CHECK(red(at_ndc(pixels, 0.5F, 0)));
    auto busy = renderer->reload_skin(harness.device);
    REQUIRE_FALSE(busy);
    CHECK(*calls == 2);
    require_ok(frame.end());

    auto failed = renderer->reload_skin(harness.device, vng::resources::provider(
        [] -> std::expected<vng::rig::SkinBinding<Mesh>, vng::rig::Diagnostic> {
            return std::unexpected(vng::rig::Diagnostic{
                .code = vng::rig::ErrorCode::invalid_mesh,
                .message = "failed replacement", .vertex = {}});
        }));
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().cause_as<vng::rig::Diagnostic>());
    auto stable = begin_clear(harness);
    require_ok(renderer->render(stable, view(), vng::render::SkinnedDraw{.pose = pose}));
    CHECK(read_pixels(harness) == pixels);
    require_ok(stable.end());
    require_ok(renderer->reload_skin(harness.device));
    CHECK(*calls == 3);
    CHECK(pose.revision() == revision);
    auto refreshed = begin_clear(harness);
    require_ok(renderer->render(refreshed, view(), vng::render::SkinnedDraw{.pose = pose}));
    CHECK(read_pixels(harness) == pixels);
    require_ok(refreshed.end());

    auto invalid_mesh = triangle();
    invalid_mesh.vertices()[0].set(vng::gfx::Position{},
        {std::numeric_limits<float>::quiet_NaN(), 0, 0});
    auto invalid_binding = vng::rig::bind(std::move(invalid_mesh), skeleton.armature);
    require_ok(invalid_binding);
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
        require_ok(invalid_binding->set_weights(vertex, {{skeleton.root, 1.0F}}));
    REQUIRE_FALSE(renderer->reload_skin(harness.device,
        vng::providers::skin(std::move(*invalid_binding))));
    require_ok(renderer->reload_skin(harness.device));
    CHECK(*calls == 4);

    require_ok(renderer->replace_skin(harness.device, binding));
    auto missing = renderer->reload_skin(harness.device);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == vng::resources::ErrorCode::no_provider);
    auto all = renderer->reload(harness.device);
    require_ok(all);
    CHECK(all->retained == std::vector<std::string>{"skin"});
    CHECK(*calls == 4);
    require_ok(renderer->replace_skin(harness.device, vng::resources::provided(binding, recipe)));
    CHECK(*calls == 4);
    require_ok(renderer->reload(harness.device));
    CHECK(*calls == 5);
    CHECK(pose.revision() == revision);
}

TEST_CASE("skin reload rejects reentrant mutation and a provider leaving a frame active",
          "[opengl][rig][resources]")
{
    auto harness = create_harness();
    auto skeleton = armature();
    auto binding = weighted_triangle(skeleton);
    vng::opengl::SkinnedMeshRenderer* owner{};
    bool open_frame{};
    std::optional<vng::opengl::Frame> borrowed;
    auto recipe = vng::resources::provider([&]()
        -> vng::resources::Result<vng::rig::SkinBinding<Mesh>> {
        if (owner) {
            CHECK_FALSE(owner->reload_skin(harness.device));
            CHECK_FALSE(owner->replace_skin(harness.device, binding));
        }
        if (open_frame) borrowed.emplace(begin_clear(harness));
        return binding;
    });
    auto builder = vng::render::skinned_mesh_renderer_builder(harness.device);
    auto renderer = builder.skin(recipe).build();
    require_ok(renderer);
    owner = &*renderer;
    require_ok(renderer->reload_skin(harness.device));
    open_frame = true;
    auto blocked = renderer->reload(harness.device);
    REQUIRE_FALSE(blocked);
    REQUIRE(borrowed);
    REQUIRE(borrowed->active());
    auto pose = skeleton.armature.rest_pose();
    require_ok(renderer->render(*borrowed, view(), vng::render::SkinnedDraw{.pose = pose}));
    CHECK(red(at_ndc(read_pixels(harness), 0, 0)));
    require_ok(borrowed->end());
    borrowed.reset();
    open_frame = false;
    require_ok(renderer->reload(harness.device));

    owner = nullptr;
    builder.skin(vng::resources::provider([&]()
        -> vng::resources::Result<vng::rig::SkinBinding<Mesh>> {
        builder.skin(vng::providers::skin(binding)).options({.max_influences = 3});
        return binding;
    }));
    auto snapshotted = builder.build();
    require_ok(snapshotted);
    require_ok(snapshotted->reload_skin(harness.device));
    REQUIRE_FALSE(builder.build());
}

TEST_CASE("four and eight GPU influences agree with CPU skinning",
          "[opengl][rig]")
{
    auto harness = create_harness();
    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using GetVertexArrayIndexedInteger = void (*)(std::uint32_t, std::uint32_t,
        std::uint32_t, std::int32_t*);
    const auto get = reinterpret_cast<GetInteger>(
        harness.access.resolve("glGetIntegerv"));
    const auto get_vertex_binding = reinterpret_cast<GetVertexArrayIndexedInteger>(
        harness.access.resolve("glGetVertexArrayIndexediv"));
    REQUIRE(get != nullptr);
    REQUIRE(get_vertex_binding != nullptr);
    for (const vng::u32 influence_count : {4U, 8U}) {
        CAPTURE(influence_count);
        vng::rig::ArmatureBuilder builder;
        const auto root = builder.add_bone("root");
        std::array<vng::rig::BoneId, 8> bones;
        for (std::size_t i = 0; i < bones.size(); ++i) {
            bones[i] = builder.add_bone("influence" + std::to_string(i), root);
        }
        auto skeleton = builder.build();
        require_ok(skeleton);
        auto binding = vng::rig::bind(triangle(), *skeleton);
        require_ok(binding);
        std::vector<vng::rig::Influence> weights;
        for (vng::u32 i = 0; i < influence_count; ++i) {
            weights.push_back({bones[i], 1.0F / static_cast<float>(influence_count)});
        }
        for (std::size_t vertex = 0; vertex < 3; ++vertex) {
            require_ok(binding->set_weights(vertex, weights));
        }
        auto renderer = vng::render::make_skinned_mesh_renderer(
            harness.device, *binding,
            vng::render::SkinnedRendererOptions{.max_influences = influence_count});
        require_ok(renderer);

        auto pose = skeleton->rest_pose();
        float expected_translation = 0.0F;
        for (vng::u32 i = 0; i < influence_count; ++i) {
            const auto delta = 0.07F * static_cast<float>(i + 1);
            expected_translation += delta / static_cast<float>(influence_count);
            require_ok(pose.set_local(bones[i],
                vng::rig::Transform{.translation = {delta, 0, 0}}));
        }
        auto cpu = binding->deform_points(triangle_positions, pose);
        require_ok(cpu);
        REQUIRE(cpu->size() == 3);
        for (std::size_t i = 0; i < cpu->size(); ++i) {
            CHECK(std::abs((*cpu)[i].x - triangle_positions[i].x
                - expected_translation) < 1.0e-5F);
        }

        auto frame = begin_clear(harness);
        require_ok(renderer->render(frame, view(),
            vng::render::SkinnedDraw{.pose = pose}));
        const auto pixels = read_pixels(harness);
        CHECK(red(at_ndc(pixels, expected_translation, 0)));
        CHECK(black(at_ndc(pixels, 0, 0)));

        // Four influences must really consume half the GPU attribute bytes,
        // not merely select a shader branch over an always-eight-wide stream.
        std::int32_t vertex_array{}, influence_stride{};
        get(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &vertex_array);
        REQUIRE(vertex_array > 0);
        get_vertex_binding(static_cast<std::uint32_t>(vertex_array), 1,
            0x82D8 /* GL_VERTEX_BINDING_STRIDE */, &influence_stride);
        CHECK(influence_stride == static_cast<std::int32_t>(influence_count * 8U));

        const auto& inputs = renderer->source().vertex.interface.inputs;
        const auto uses_second_joint_set = std::ranges::any_of(inputs,
            [](const auto& input) {
                return input.semantic_type == typeid(vng::gfx::JointIndices<1>);
            });
        const auto uses_second_weight_set = std::ranges::any_of(inputs,
            [](const auto& input) {
                return input.semantic_type == typeid(vng::gfx::JointWeights<1>);
            });
        CHECK(uses_second_joint_set == (influence_count == 8));
        CHECK(uses_second_weight_set == (influence_count == 8));
    }
}

TEST_CASE("rigged rendering owns geometry and weight snapshots",
          "[opengl][rig]")
{
    auto harness = create_harness();
    auto skeleton = armature();
    auto mesh = triangle();
    auto binding = vng::rig::bind(mesh, skeleton.armature);
    require_ok(binding);
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
        require_ok(binding->set_weights(vertex, {{skeleton.root, 1.0F}}));
    }
    // First edit the source before upload: binding must already own geometry.
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
        mesh.vertices()[vertex].set(vng::gfx::Position{}, {8, 8, 8});
    }
    auto renderer = vng::render::make_skinned_mesh_renderer(harness.device, *binding);
    require_ok(renderer);

    // Then edit authored weights after upload: renderer must own those too.
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
        require_ok(binding->set_weights(vertex, {{skeleton.child, 1.0F}}));
    }
    auto pose = skeleton.armature.rest_pose();
    require_ok(pose.set_local(skeleton.child,
        vng::rig::Transform{.translation = {0.5F, 0, 0}}));
    auto frame = begin_clear(harness);
    require_ok(renderer->render(frame, view(), vng::render::SkinnedDraw{.pose = pose}));
    const auto pixels = read_pixels(harness);
    CHECK(red(at_ndc(pixels, 0, 0)));
    CHECK(black(at_ndc(pixels, 0.5F, 0)));
}

TEST_CASE("skinned tickets accept independent poses and placements",
          "[opengl][rig]")
{
    auto harness = create_harness();
    auto skeleton = armature();
    auto binding = weighted_triangle(skeleton);
    auto renderer = vng::render::make_skinned_mesh_renderer(harness.device, binding);
    require_ok(renderer);
    auto left = skeleton.armature.rest_pose();
    auto right = skeleton.armature.rest_pose();
    require_ok(left.set_local(skeleton.root,
        vng::rig::Transform{.translation = {-0.25F, 0, 0}}));
    require_ok(right.set_local(skeleton.root,
        vng::rig::Transform{.translation = {0.25F, 0, 0}}));
    const std::array draws{
        vng::render::SkinnedDraw{
            .pose = left, .transform = {.translation = {-0.25F, 0, 0}},
        },
        vng::render::SkinnedDraw{
            .pose = right, .transform = {.translation = {0.25F, 0, 0}},
        },
    };
    auto frame = begin_clear(harness);
    require_ok(renderer->render(frame, view(), draws));
    const auto pixels = read_pixels(harness);
    CHECK(red(at_ndc(pixels, -0.5F, 0)));
    CHECK(red(at_ndc(pixels, 0.5F, 0)));
    CHECK(black(at_ndc(pixels, 0, 0)));
    CHECK(renderer->stats().draw_calls == 2);
    CHECK(renderer->stats().triangles == 2);

    auto foreign = armature().armature.rest_pose();
    auto rejected = renderer->render(frame, view(),
        vng::render::SkinnedDraw{.pose = foreign});
    CHECK_FALSE(rejected);
    CHECK(read_pixels(harness) == pixels);
    auto no_camera = renderer->render(frame,
        vng::render::RenderView::without_camera(extent),
        vng::render::SkinnedDraw{.pose = left});
    CHECK_FALSE(no_camera);
}

TEST_CASE("placement arguments do not reupload an unchanged bone palette",
          "[opengl][rig][arguments]")
{
    auto harness = create_harness();
    auto skeleton = armature();
    auto renderer = vng::render::make_skinned_mesh_renderer(harness.device, weighted_triangle(skeleton));
    require_ok(renderer);
    auto pose = skeleton.armature.rest_pose();
    auto first = begin_clear(harness);
    require_ok(renderer->render(first, view(), vng::render::SkinnedDraw{.pose = pose}));
    CHECK(renderer->stats().palette_uploads == 1);
    CHECK(red(at_ndc(read_pixels(harness), 0, 0)));
    require_ok(first.end());

    auto moved = begin_clear(harness);
    const vng::render::SkinnedDraw draw{
        .pose = pose, .transform = {.translation = {0.5F, 0, 0}}};
    require_ok(renderer->render(moved, view(), draw));
    CHECK(renderer->stats().palette_uploads == 0);
    const auto pixels = read_pixels(harness);
    CHECK(black(at_ndc(pixels, 0, 0)));
    CHECK(red(at_ndc(pixels, 0.5F, 0)));
}

TEST_CASE("enhanced skinned rendering uses the same deformation and placement",
          "[opengl][rig]")
{
    auto harness = create_harness();
    auto skeleton = armature();
    using AuthoredNormals = vng::gfx::Record<vng::gfx::Normal>;
    vng::gfx::Mesh<Vertex, AuthoredNormals> mesh{3};
    auto geometry = triangle();
    mesh.vertices<Vertex>() = std::move(geometry.vertices());
    mesh.faces() = std::move(geometry.faces());
    for (auto& vertex : mesh.vertices<AuthoredNormals>()) {
        vertex.set(vng::gfx::Normal{}, {0, 0, 1});
    }
    auto binding = vng::rig::bind(std::move(mesh), skeleton.armature);
    require_ok(binding);
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
        require_ok(binding->set_weights(vertex, {{skeleton.root, 1.0F}}));
    }
    // Normals are genuinely authored and remain observable even when the
    // ordinary color shader deliberately disables lighting.
    auto renderer = vng::render::make_skinned_mesh_renderer(harness.device, *binding,
        vng::render::SkinnedRendererOptions{.lighting = false});
    require_ok(renderer);
    auto pose = skeleton.armature.rest_pose();
    require_ok(pose.set_local(skeleton.root,
        vng::rig::Transform{.translation = {0.25F, 0.125F, 0}}));
    const vng::render::SkinnedDraw draw{
        .pose = pose, .transform = {.translation = {0.25F, 0.125F, 0}},
    };
    auto frame = begin_clear(harness);
    require_ok(renderer->render(frame, view(), draw));
    const auto normal = read_pixels(harness);
    CHECK(red(at_ndc(normal, 0.5F, 0.25F)));
    auto request = vng::analysis::CaptureRequest::standard()
        .observe(vng::render::SkinnedWorldPosition{})
        .observe(vng::render::SkinnedWorldNormal{});
    auto caller = frame.commands();
    require_ok(caller.graphics_state().set(vng::render::BlendMode::additive));
    require_ok(caller.graphics_state().set(vng::opengl::PolygonMode::line));
    require_ok(caller.graphics_state().set(vng::render::DepthCompare::never));
    using GetNativeInteger = void (*)(std::uint32_t, std::int32_t*);
    const auto get_native = reinterpret_cast<GetNativeInteger>(
        harness.access.resolve("glGetIntegerv"));
    REQUIRE(get_native != nullptr);
    const auto native_raster = [&] {
        std::array<std::int32_t, 4> values{};
        get_native(0x0B40, values.data()); // GL_POLYGON_MODE: front and back
        get_native(0x0BE2, &values[2]); // GL_BLEND
        get_native(0x0B74, &values[3]); // GL_DEPTH_FUNC
        return values;
    };
    const auto caller_raster = native_raster();
    auto evidence = renderer->capture(frame, view(), draw, request);
    require_ok(evidence);
    CHECK(native_raster() == caller_raster);
    CHECK(caller.active());
    const auto& capture = evidence->capture();
    CHECK(capture.extent() == extent);
    CHECK(capture.summary().covered_pixel_count > 100);
    CHECK(capture.summary().visible_primitive_count == 1);
    int max_rgb_difference = 0;
    std::size_t mismatched_coverage = 0;
    for (vng::u32 y = 0; y < extent.height; ++y) {
        for (vng::u32 x = 0; x < extent.width; ++x) {
            // Evidence rows are top-left; raw OpenGL readback is bottom-left.
            const vng::analysis::Pixel coordinate{x, extent.height - 1 - y};
            const auto& expected = normal[y * extent.width + x];
            const auto color = capture.color().at(coordinate);
            const std::array<vng::u8, 3> rgb{color.r, color.g, color.b};
            for (std::size_t component = 0; component < rgb.size(); ++component) {
                max_rgb_difference = std::max(max_rgb_difference,
                    std::abs(static_cast<int>(expected[component])
                        - static_cast<int>(rgb[component])));
            }
            mismatched_coverage += red(expected)
                != capture.surface_keys().at(coordinate).has_surface();
        }
    }
    CHECK(max_rgb_difference <= 1);
    CHECK(mismatched_coverage == 0);
    // Diagnostic rendering is target-isolated; it must preserve the already
    // rendered ordinary framebuffer as well as restore its binding.
    CHECK(read_pixels(harness) == normal);

    const auto* positions = evidence->observation<vng::render::SkinnedWorldPosition>();
    const auto* normals = evidence->observation<vng::render::SkinnedWorldNormal>();
    REQUIRE(positions != nullptr);
    REQUIRE(normals != nullptr);
    const vng::analysis::Pixel center{96, 47};
    REQUIRE(capture.surface_keys().at(center).has_surface());
    CHECK(std::abs(positions->at(center).x - 0.5078125F) < 1.0e-4F);
    CHECK(std::abs(positions->at(center).y - 0.2578125F) < 1.0e-4F);
    CHECK(std::abs(positions->at(center).z) < 1.0e-4F);
    CHECK(std::abs(normals->at(center).x) < 1.0e-4F);
    CHECK(std::abs(normals->at(center).y) < 1.0e-4F);
    CHECK(std::abs(normals->at(center).z - 1.0F) < 1.0e-4F);

    // Topology, shader, and placement are unchanged. Pose-resource contents
    // still make this a different workload, not a valid raster counterfactual.
    require_ok(pose.set_local(skeleton.root,
        vng::rig::Transform{.translation = {0.375F, 0.125F, 0}}));
    auto second = renderer->capture(frame, view(), draw);
    require_ok(second);
    REQUIRE(evidence->metadata().invocation);
    REQUIRE(second->metadata().invocation);
    CHECK(evidence->metadata().invocation->workload_fingerprint
        != second->metadata().invocation->workload_fingerprint);
}

TEST_CASE("skinned geometry roles accept custom position-only records",
          "[opengl][rig]")
{
    struct Coordinate : vng::gfx::Semantic<vng::Vec3> {};
    using CustomVertex = vng::gfx::Record<Coordinate>;
    auto harness = create_harness();
    auto skeleton = armature();
    vng::gfx::Mesh<CustomVertex> mesh{3};
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
        mesh.vertices()[vertex].set(Coordinate{}, triangle_positions[vertex]);
    }
    mesh.add_face(0, 1, 2);
    auto binding = vng::rig::bind(mesh, skeleton.armature);
    require_ok(binding);
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
        require_ok(binding->set_weights(vertex, {{skeleton.root, 1.0F}}));
    }
    auto renderer = vng::render::make_skinned_mesh_renderer(harness.device, *binding,
        vng::render::GeometryFields{.position = Coordinate{}});
    require_ok(renderer);
    auto pose = skeleton.armature.rest_pose();
    auto frame = begin_clear(harness);
    const vng::render::SkinnedDraw draw{.pose = pose};
    require_ok(renderer->render(frame, view(), draw));
    const auto production = read_pixels(harness);
    const auto pixel = at_ndc(production, 0, 0);
    CHECK(pixel[0] > 240);
    CHECK(pixel[1] > 240);
    CHECK(pixel[2] > 240);

    // The internal default direction enables a common vertex representation;
    // it is not authored surface data and must not masquerade as evidence.
    auto missing_normal = renderer->capture(frame, view(), draw,
        vng::analysis::CaptureRequest::standard()
            .observe(vng::render::SkinnedWorldNormal{}));
    CHECK_FALSE(missing_normal);
    auto positions = renderer->capture(frame, view(), draw,
        vng::analysis::CaptureRequest::standard()
            .observe(vng::render::SkinnedWorldPosition{}));
    require_ok(positions);
    const auto* world = positions->observation<vng::render::SkinnedWorldPosition>();
    REQUIRE(world != nullptr);
    const vng::analysis::Pixel center{64, 63};
    REQUIRE(positions->capture().surface_keys().at(center).has_surface());
    CHECK(std::abs(world->at(center).x - 0.0078125F) < 1.0e-4F);
    CHECK(std::abs(world->at(center).y - 0.0078125F) < 1.0e-4F);
    CHECK(std::abs(world->at(center).z) < 1.0e-4F);
    CHECK(read_pixels(harness) == production);
}

TEST_CASE("skinned normals remain unit length for extreme source and transform magnitudes",
          "[opengl][rig][normal]")
{
    using LitVertex = vng::gfx::Record<
        vng::gfx::Position, vng::gfx::Normal, vng::gfx::Color>;
    struct Case final {
        float position_scale;
        float source_normal_z;
        float placement_scale;
    };
    constexpr std::array cases{
        // Raw dot(normal, normal) would overflow a float. The asset boundary
        // should remove source magnitude before passing the direction to GPU.
        Case{1.0F, 1.0e30F, 1.0F},
        // Inverse-transpose scaling makes a valid world normal very small.
        // Fixed epsilon clamping before normalization must not darken it.
        Case{1.0e-12F, 1.0F, 1.0e12F},
    };
    auto harness = create_harness();
    auto skeleton = armature();
    auto pose = skeleton.armature.rest_pose();
    for (const auto& value : cases) {
        CAPTURE(value.position_scale, value.source_normal_z, value.placement_scale);
        vng::gfx::Mesh<LitVertex> mesh{3};
        for (std::size_t vertex = 0; vertex < 3; ++vertex) {
            auto position = triangle_positions[vertex];
            for (std::size_t component = 0; component < 3; ++component) {
                position[component] *= value.position_scale;
            }
            mesh.vertices()[vertex].set(vng::gfx::Position{}, position);
            mesh.vertices()[vertex].set(vng::gfx::Normal{}, {0, 0, value.source_normal_z});
            mesh.vertices()[vertex].set(vng::gfx::Color{}, {1, 1, 1, 1});
        }
        mesh.add_face(0, 1, 2);
        auto binding = vng::rig::bind(std::move(mesh), skeleton.armature);
        require_ok(binding);
        for (std::size_t vertex = 0; vertex < 3; ++vertex) {
            require_ok(binding->set_weights(vertex, {{skeleton.root, 1.0F}}));
        }
        auto renderer = vng::render::make_skinned_mesh_renderer(harness.device, *binding);
        require_ok(renderer);
        auto frame = begin_clear(harness);
        const vng::render::SkinnedDraw draw{
            .pose = pose, .transform = {.scale = value.placement_scale},
        };
        auto captured = renderer->capture(frame, view(), draw,
            vng::analysis::CaptureRequest::standard()
                .observe(vng::render::SkinnedWorldNormal{}));
        require_ok(captured);
        const auto* normals = captured->observation<vng::render::SkinnedWorldNormal>();
        REQUIRE(normals != nullptr);
        const vng::analysis::Pixel center{64, 63};
        REQUIRE(captured->capture().surface_keys().at(center).has_surface());
        const auto normal = normals->at(center);
        CHECK(std::abs(normal.x) < 1.0e-4F);
        CHECK(std::abs(normal.y) < 1.0e-4F);
        CHECK(std::abs(normal.z - 1.0F) < 1.0e-4F);
        CHECK(captured->capture().color().at(center).r > 200);
    }
}

TEST_CASE("skinned rendering restores native storage-buffer ranges and generic binding",
          "[opengl][rig]")
{
    auto harness = create_harness();
    auto skeleton = armature();
    auto binding = weighted_triangle(skeleton);
    auto renderer = vng::render::make_skinned_mesh_renderer(harness.device, binding);
    require_ok(renderer);
    auto pose = skeleton.armature.rest_pose();

    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using GetIndexedInteger = void (*)(std::uint32_t, std::uint32_t, std::int32_t*);
    using GetIndexedInteger64 = void (*)(std::uint32_t, std::uint32_t, std::int64_t*);
    using BindBuffer = void (*)(std::uint32_t, std::uint32_t);
    using BindBufferRange = void (*)(std::uint32_t, std::uint32_t, std::uint32_t,
        std::intptr_t, std::intptr_t);
    const auto get = reinterpret_cast<GetInteger>(harness.access.resolve("glGetIntegerv"));
    const auto get_indexed = reinterpret_cast<GetIndexedInteger>(harness.access.resolve("glGetIntegeri_v"));
    const auto get_indexed64 = reinterpret_cast<GetIndexedInteger64>(harness.access.resolve("glGetInteger64i_v"));
    const auto bind_buffer = reinterpret_cast<BindBuffer>(harness.access.resolve("glBindBuffer"));
    const auto bind_range = reinterpret_cast<BindBufferRange>(harness.access.resolve("glBindBufferRange"));
    REQUIRE(get != nullptr);
    REQUIRE(get_indexed != nullptr);
    REQUIRE(get_indexed64 != nullptr);
    REQUIRE(bind_buffer != nullptr);
    REQUIRE(bind_range != nullptr);
    constexpr std::uint32_t storage_buffer = 0x90D2;
    constexpr std::uint32_t storage_binding = 0x90D3;
    constexpr std::uint32_t storage_start = 0x90D4;
    constexpr std::uint32_t storage_size = 0x90D5;
    constexpr std::uint32_t storage_alignment = 0x90DF;
    std::int32_t alignment{};
    get(storage_alignment, &alignment);
    REQUIRE(alignment > 0);
    const auto offset = static_cast<std::intptr_t>(alignment);
    const auto size = static_cast<std::intptr_t>(alignment) * 2;
    auto indexed = vng::opengl::Buffer::create(harness.device,
        {.size = static_cast<std::size_t>(offset + size)});
    auto generic = vng::opengl::Buffer::create(harness.device, {.size = 64});
    require_ok(indexed);
    require_ok(generic);
    bind_range(storage_buffer, 0, indexed->native_handle(), offset, size);
    bind_buffer(storage_buffer, generic->native_handle());

    const auto check_binding = [&] {
        std::int32_t actual_indexed{}, actual_generic{};
        std::int64_t actual_start{}, actual_size{};
        get_indexed(storage_binding, 0, &actual_indexed);
        get(storage_binding, &actual_generic);
        get_indexed64(storage_start, 0, &actual_start);
        get_indexed64(storage_size, 0, &actual_size);
        CHECK(static_cast<std::uint32_t>(actual_indexed) == indexed->native_handle());
        CHECK(static_cast<std::uint32_t>(actual_generic) == generic->native_handle());
        CHECK(actual_start == offset);
        CHECK(actual_size == size);
    };

    auto frame = begin_clear(harness);
    const vng::render::SkinnedDraw draw{.pose = pose};
    require_ok(renderer->render(frame, view(), draw));
    check_binding();
    CHECK(red(at_ndc(read_pixels(harness), 0, 0)));
    auto captured = renderer->capture(frame, view(), draw);
    require_ok(captured);
    check_binding();
}

TEST_CASE("buffer upload reports native mapped-storage failure before cacheable success",
          "[opengl][buffer][rig][regression]")
{
    auto harness = create_harness();
    auto buffer = vng::opengl::Buffer::create(harness.device, {
        .size = 16,
        .storage = vng::opengl::BufferStorage::dynamic
            | vng::opengl::BufferStorage::map_write,
    });
    require_ok(buffer);

    using MapBuffer = void* (*)(std::uint32_t, std::intptr_t, std::intptr_t, std::uint32_t);
    using UnmapBuffer = std::uint8_t (*)(std::uint32_t);
    const auto map = reinterpret_cast<MapBuffer>(
        harness.access.resolve("glMapNamedBufferRange"));
    const auto unmap = reinterpret_cast<UnmapBuffer>(
        harness.access.resolve("glUnmapNamedBuffer"));
    REQUIRE(map != nullptr);
    REQUIRE(unmap != nullptr);
    auto* mapped = map(buffer->native_handle(), 0, 16, 0x0002 /* GL_MAP_WRITE_BIT */);
    REQUIRE(mapped != nullptr);
    struct MappingScope final {
        UnmapBuffer unmap;
        std::uint32_t buffer;
        ~MappingScope() { if (buffer != 0) unmap(buffer); }
    } mapping{unmap, buffer->native_handle()};

    const std::array update{std::byte{0x5A}};
    // This byte range and storage flags pass all C++ checks. Native OpenGL
    // rejects glNamedBufferSubData while non-persistent mapping is active.
    // A renderer must see this failure rather than cache the upload as done.
    auto failed_write = buffer->write(0, update);
    CHECK_FALSE(failed_write);
    if (!failed_write) {
        CHECK(failed_write.error().code == vng::opengl::ErrorCode::operation_failed);
        CHECK(failed_write.error().message.find("GL_INVALID_OPERATION") != std::string::npos);
    }

    const auto unmapped = unmap(buffer->native_handle());
    mapping.buffer = 0;
    REQUIRE(unmapped != 0);
    require_ok(buffer->write(0, update));
    std::array<std::byte, 1> readback{};
    require_ok(buffer->read(0, readback));
    CHECK(readback == update);
}
