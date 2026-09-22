#include <vng/gfx/gfx.hpp>
#include <vng/render_opengl/render_opengl.hpp>
#include <vng/shader/shader.hpp>
#include <vng/glfw_opengl/glfw_opengl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Position3D : vng::gfx::Semantic<vng::Vec3> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};
struct ObservedTint : vng::gfx::Semantic<vng::Vec3> {};
struct ObservedCounter : vng::gfx::Semantic<vng::u32> {};
struct MissingObservation : vng::gfx::Semantic<vng::Vec4> {};

using Vertex = vng::gfx::Record<
    Position,
    vng::gfx::as<Color, vng::gfx::unorm8x4>>;
using PositionOnlyVertex = vng::gfx::Record<Position>;
using VertexInputs = vng::shader::VertexInputs<Position, Color>;
using VertexOutputs = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<Color>>;
using FragmentInputs = vng::shader::FragmentInputs<
    vng::shader::smooth<Color>>;
using FragmentOutputs = vng::shader::FragmentOutputs<
    vng::shader::Color<0>>;
using SparseFragmentOutputs = vng::shader::FragmentOutputs<
    vng::shader::Color<0>,
    vng::shader::Color<7>>;
using CameraVertex = vng::gfx::Record<Position3D, Color>;
using CameraVertexInputs = vng::shader::VertexInputs<Position3D, Color>;

[[noreturn]] void skip_ctest(const std::string& reason)
{
    std::cerr << "OpenGL renderer test skipped: " << reason << '\n';
    std::exit(77);
}

[[nodiscard]] std::string describe(const vng::opengl::Diagnostic& diagnostic)
{
    auto result = diagnostic.message;
    if (!diagnostic.driver_log.empty()) {
        result += "\ndriver log:\n" + diagnostic.driver_log;
    }
    if (!diagnostic.generated_source.empty()) {
        result += "\ngenerated source:\n" + diagnostic.generated_source;
    }
    return result;
}

[[nodiscard]] vng::opengl::CaptureState capture_state(
    vng::opengl::GraphicsStateSnapshot raster = {}) noexcept
{
    return {raster, vng::render::ColorEncoding::linear};
}

} // namespace

TEST_CASE("renderer emits and runs an enhanced analysis shader automatically",
          "[render][opengl][analysis]")
{
    auto window = vng::glfw_opengl::create_window(
        vng::window::WindowDesc{
            .width = 32,
            .height = 32,
            .title = "vng hidden analysis renderer test",
            .visible = false,
            .resizable = false,
        },
        vng::opengl::ContextDesc{
            .debug = true,
            .samples = 0,
            .default_framebuffer_encoding =
                vng::render::ColorEncoding::linear,
        }, {.vsync = vng::window::VSync::off});
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest(
            "OpenGL context could not be made current: "
            + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device);

    auto vertex = vng::shader::vertex<VertexInputs, VertexOutputs>(
        "analysis_renderer_vertex",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    vng::dsl::vec4(stage.input(Position{}), 0.0F, 1.0F)),
                vng::dsl::field<Color>(stage.input(Color{})));
        });
    REQUIRE(vertex);
    auto fragment = vng::shader::fragment<FragmentInputs, FragmentOutputs>(
        "analysis_renderer_fragment",
        [](auto& stage) {
            const auto color = stage.input(Color{});
            stage.observe(
                ObservedTint{},
                color.xyz() + vng::Vec3{0.125F, 0.25F, 0.375F});
            stage.observe(
                ObservedCounter{},
                stage.constant(vng::u32{37}));
            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(
                    color));
        });
    REQUIRE(fragment);
    auto shader_program = vng::shader::link(
        std::move(*vertex), std::move(*fragment));
    REQUIRE(shader_program);
    const auto original_vertex_ir = shader_program->vertex().dump_ir();
    const auto original_fragment_ir = shader_program->fragment().dump_ir();

    auto renderer = vng::render::OpenGLProgramRuntime::create(
        *device, *shader_program);
    INFO((renderer ? std::string{} : describe(renderer.error())));
    REQUIRE(renderer);
    CHECK(renderer->analysis_source() == nullptr);

    auto normal_only_renderer = vng::render::OpenGLProgramRuntime::create(
        *device,
        *shader_program);
    INFO((normal_only_renderer
        ? std::string{}
        : describe(normal_only_renderer.error())));
    REQUIRE(normal_only_renderer);
    CHECK(normal_only_renderer->analysis_source() == nullptr);

    vng::gfx::Mesh<Vertex> mesh(6);
    mesh.vertices()[0].set(Position{}, {-0.8F, -0.8F});
    mesh.vertices()[1].set(Position{}, {0.8F, -0.8F});
    mesh.vertices()[2].set(Position{}, {0.0F, 0.8F});
    mesh.vertices()[3].set(Position{}, {0.75F, 0.2F});
    mesh.vertices()[4].set(Position{}, {0.95F, 0.2F});
    mesh.vertices()[5].set(Position{}, {0.85F, 0.7F});
    for (std::size_t index = 0; index < 3; ++index) {
        mesh.vertices()[index].set(
            Color{}, {1.0F, 0.0F, 0.0F, 1.0F});
    }
    for (std::size_t index = 3; index < 6; ++index) {
        mesh.vertices()[index].set(
            Color{}, {0.0F, 1.0F, 0.0F, 1.0F});
    }
    mesh.add_face(0, 1, 2);
    mesh.add_face(3, 4, 5);

    auto gpu_mesh = vng::opengl::upload_mesh(*device, mesh);
    INFO((gpu_mesh ? std::string{} : describe(gpu_mesh.error())));
    REQUIRE(gpu_mesh);

    auto unsupported_analysis = normal_only_renderer->render_analysis(
        *device, mesh, *gpu_mesh, {32, 32},capture_state(vng::opengl::GraphicsStateSnapshot{
            .depth = {
                .test = true,
                .write = false,
                .compare = vng::render::DepthCompare::less,
            },
        }));
    REQUIRE_FALSE(unsupported_analysis);
    CHECK(unsupported_analysis.error().code
          == vng::opengl::ErrorCode::invalid_argument);
    CHECK(normal_only_renderer->analysis_source() == nullptr);

    // A CPU mesh that merely has the same number of faces is not sufficient:
    // source-face provenance must describe the topology actually uploaded.
    auto wrong_source_mesh = mesh;
    std::swap(
        wrong_source_mesh.faces()[0][1],
        wrong_source_mesh.faces()[0][2]);
    auto mismatched = renderer->render_analysis(
        *device, wrong_source_mesh, *gpu_mesh, {32, 32},capture_state());
    REQUIRE_FALSE(mismatched);
    CHECK(mismatched.error().code
          == vng::opengl::ErrorCode::invalid_argument);

    // Deliberately leave a caller-owned target selected. Analysis rendering
    // must restore both it and its viewport on every successful capture.
    auto caller_color = vng::opengl::Image2D::create(
        *device, 16, 16, vng::opengl::ImageFormat::rgba8);
    REQUIRE(caller_color);
    auto caller_framebuffer = vng::opengl::Framebuffer::create(*device);
    REQUIRE(caller_framebuffer);
    REQUIRE(caller_framebuffer->attach_color(0, *caller_color));
    REQUIRE(caller_framebuffer->check_complete());
    REQUIRE(caller_framebuffer->bind());
    REQUIRE(device->viewport(0, 0, 16, 16));
    REQUIRE(caller_color->clear_rgba8({0.0F, 0.0F, 0.0F, 1.0F}));

    // Seed hostile but legal caller state. The analysis path must neutralize
    // it for the draw and restore it afterwards.
    using Capability = void (*)(std::uint32_t);
    using IsEnabled = std::uint8_t (*)(std::uint32_t);
    using LogicOp = void (*)(std::uint32_t);
    using PolygonMode = void (*)(std::uint32_t, std::uint32_t);
    using SampleMask = void (*)(std::uint32_t, std::uint32_t);
    using ClipControl = void (*)(std::uint32_t, std::uint32_t);
    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using GetBooleanIndexed = void (*)(
        std::uint32_t, std::uint32_t, std::uint8_t*);
    using IsEnabledIndexed = std::uint8_t (*)(
        std::uint32_t, std::uint32_t);
    const auto enable = reinterpret_cast<Capability>(
        access->resolve("glEnable"));
    const auto is_enabled = reinterpret_cast<IsEnabled>(
        access->resolve("glIsEnabled"));
    const auto logic_op = reinterpret_cast<LogicOp>(
        access->resolve("glLogicOp"));
    const auto polygon_mode = reinterpret_cast<PolygonMode>(
        access->resolve("glPolygonMode"));
    const auto sample_mask = reinterpret_cast<SampleMask>(
        access->resolve("glSampleMaski"));
    const auto clip_control = reinterpret_cast<ClipControl>(
        access->resolve("glClipControl"));
    const auto get_integer = reinterpret_cast<GetInteger>(
        access->resolve("glGetIntegerv"));
    const auto get_boolean_indexed = reinterpret_cast<GetBooleanIndexed>(
        access->resolve("glGetBooleani_v"));
    const auto is_enabled_indexed = reinterpret_cast<IsEnabledIndexed>(
        access->resolve("glIsEnabledi"));
    REQUIRE(enable != nullptr);
    REQUIRE(is_enabled != nullptr);
    REQUIRE(logic_op != nullptr);
    REQUIRE(polygon_mode != nullptr);
    REQUIRE(sample_mask != nullptr);
    REQUIRE(clip_control != nullptr);
    REQUIRE(get_integer != nullptr);
    REQUIRE(get_boolean_indexed != nullptr);
    REQUIRE(is_enabled_indexed != nullptr);

    constexpr std::uint32_t gl_color_logic_op = 0x0BF2;
    constexpr std::uint32_t gl_xor = 0x1506;
    constexpr std::uint32_t gl_polygon_mode = 0x0B40;
    constexpr std::uint32_t gl_front_and_back = 0x0408;
    constexpr std::uint32_t gl_line = 0x1B01;
    constexpr std::uint32_t gl_sample_mask = 0x8E51;
    constexpr std::uint32_t gl_clip_distance0 = 0x3000;
    constexpr std::uint32_t gl_clip_origin = 0x935C;
    constexpr std::uint32_t gl_upper_left = 0x8CA2;
    constexpr std::uint32_t gl_zero_to_one = 0x935F;
    constexpr std::uint32_t gl_blend = 0x0BE2;
    constexpr std::uint32_t gl_color_writemask = 0x0C23;
    enable(gl_color_logic_op);
    logic_op(gl_xor);
    polygon_mode(gl_front_and_back, gl_line);
    enable(gl_sample_mask);
    sample_mask(0, 0);
    enable(gl_clip_distance0);
    clip_control(gl_upper_left, gl_zero_to_one);

    namespace a = vng::analysis;
    auto capture = renderer->render_analysis(
        *device,
        mesh,
        *gpu_mesh,
        {32, 32},capture_state(),
        vng::render::AnalysisOptions{
            .provenance = {
                .entity = a::EntityId{11},
                .mesh = a::MeshAssetId{22},
                .mesh_revision = a::AssetRevision{3},
                .material = a::MaterialId{44},
            },
            .label = {},
        });
    INFO((capture ? std::string{} : describe(capture.error())));
    REQUIRE(capture);
    const auto* analysis_source = renderer->analysis_source();
    REQUIRE(analysis_source != nullptr);
    REQUIRE(analysis_source->analysis);
    CHECK(analysis_source->vertex.source.find(
              "vng_analysis_first_item_id") != std::string::npos);
    CHECK(analysis_source->fragment.source.find(
              "gl_PrimitiveID") != std::string::npos);

    const auto center = capture->surface_at({16, 16});
    REQUIRE(center);
    CHECK(center->key.item == a::FrameItemId{1});
    CHECK(center->key.primitive == a::PrimitiveId{0});
    CHECK(center->entity == a::EntityId{11});
    CHECK(center->mesh == a::MeshAssetId{22});
    CHECK(center->mesh_revision == a::AssetRevision{3});
    CHECK(center->material == a::MaterialId{44});
    CHECK(center->face == 0);
    CHECK(center->vertices == vng::gfx::TriangleFace{0, 1, 2});
    CHECK(center->color.r > 245);
    CHECK(center->color.g < 10);
    CHECK(center->color.b < 10);
    CHECK(std::abs(center->device_depth - 0.5F) < 0.001F);
    CHECK_FALSE(capture->surface_at({0, 31}));
    CHECK(capture->surface_keys().at({0, 31})
          == a::SurfaceKey::background());

    // The default analysis clear follows the depth convention. A reverse-Z
    // pipeline must infer zero, otherwise every ordinary fragment would fail
    // against a clear value of one.
    auto reverse_depth_renderer = vng::render::OpenGLProgramRuntime::create(
        *device,
        *shader_program);
    INFO((reverse_depth_renderer
        ? std::string{}
        : describe(reverse_depth_renderer.error())));
    REQUIRE(reverse_depth_renderer);
    auto reverse_depth_capture = reverse_depth_renderer->render_analysis(
        *device, mesh, *gpu_mesh, {32, 32},capture_state(vng::opengl::GraphicsStateSnapshot{
            .depth = {
                .test = true,
                .write = true,
                .compare = vng::render::DepthCompare::greater_equal,
            },
        }));
    INFO((reverse_depth_capture
        ? std::string{}
        : describe(reverse_depth_capture.error())));
    REQUIRE(reverse_depth_capture);
    CHECK(reverse_depth_capture->surface_at({16, 16}).has_value());

    CHECK(is_enabled(gl_color_logic_op) != 0);
    CHECK(is_enabled(gl_sample_mask) != 0);
    CHECK(is_enabled(gl_clip_distance0) != 0);
    std::array<std::int32_t, 2> restored_polygon_modes{};
    get_integer(gl_polygon_mode, restored_polygon_modes.data());
    CHECK(static_cast<std::uint32_t>(restored_polygon_modes[0]) == gl_line);
    std::int32_t restored_clip_origin = 0;
    get_integer(gl_clip_origin, &restored_clip_origin);
    CHECK(static_cast<std::uint32_t>(restored_clip_origin) == gl_upper_left);

    const auto second_face = capture->surface_at({29, 10});
    REQUIRE(second_face);
    CHECK(second_face->key.primitive == a::PrimitiveId{1});
    CHECK(second_face->face == 1);
    CHECK(second_face->vertices == vng::gfx::TriangleFace{3, 4, 5});
    CHECK(second_face->color.r < 10);
    CHECK(second_face->color.g > 245);

    REQUIRE(device->set_standard_raster_state());
    REQUIRE(device->set_depth_state({false, false}));
    REQUIRE(device->set_cull_state({vng::opengl::CullMode::none}));
    REQUIRE(device->set_blend_enabled(0, false));
    REQUIRE(device->set_color_write_mask(0, {true, true, true, true}));
    REQUIRE(device->set_framebuffer_srgb_enabled(false));
    REQUIRE(renderer->production().bind());
    REQUIRE(gpu_mesh->draw_bound(
        *device, renderer->production()));
    auto ordinary_pixels = caller_framebuffer->read_rgba8_pixels(
        0, 0, 0, 16, 16);
    REQUIRE(ordinary_pixels);
    const auto ordinary_center = ordinary_pixels->at(8U * 16U + 8U);
    CHECK(ordinary_center.r == center->color.r);
    CHECK(ordinary_center.g == center->color.g);
    CHECK(ordinary_center.b == center->color.b);
    CHECK(ordinary_center.a == center->color.a);

    auto second_capture = renderer->render_analysis(
        *device, mesh, *gpu_mesh, {24, 20},capture_state());
    INFO((second_capture
        ? std::string{}
        : describe(second_capture.error())));
    REQUIRE(second_capture);
    CHECK(second_capture->extent() == a::Extent2D{24, 20});
    CHECK(second_capture->surface_at({12, 10}).has_value());
    CHECK(second_capture->frame().value == capture->frame().value + 1);

    // Creating and using the enhanced product never mutates or rebuilds the
    // canonical shader program.
    CHECK(shader_program->vertex().dump_ir() == original_vertex_ir);
    CHECK(shader_program->fragment().dump_ir() == original_fragment_ir);

    // Sparse user MRT locations are compiled and run by a real driver; the
    // generated surface key occupies the first hole rather than max + 1.
    auto sparse_vertex = vng::shader::vertex<VertexInputs, VertexOutputs>(
        "analysis_sparse_vertex",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    vng::dsl::vec4(stage.input(Position{}), 0.0F, 1.0F)),
                vng::dsl::field<Color>(stage.input(Color{})));
        });
    REQUIRE(sparse_vertex);
    auto sparse_fragment = vng::shader::fragment<
        FragmentInputs,
        SparseFragmentOutputs>(
        "analysis_sparse_fragment",
        [](auto& stage) {
            const auto color = stage.input(Color{});
            stage.observe(ObservedTint{}, color.xyz());
            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(color),
                vng::dsl::field<vng::shader::Color<7>>(color));
        });
    REQUIRE(sparse_fragment);
    auto sparse_program = vng::shader::link(
        std::move(*sparse_vertex), std::move(*sparse_fragment));
    REQUIRE(sparse_program);
    auto sparse_renderer = vng::render::OpenGLProgramRuntime::create(
        *device, *sparse_program);
    INFO((sparse_renderer
        ? std::string{}
        : describe(sparse_renderer.error())));
    REQUIRE(sparse_renderer);

    constexpr std::uint32_t sparse_output_location = 7;
    constexpr std::uint32_t diagnostic_output_location = 1;
    constexpr std::uint32_t untouched_location = 5;
    const std::array<bool, 4> sparse_analysis_mask{
        false, true, false, true};
    const std::array<bool, 4> diagnostic_analysis_mask{
        false, false, true, true};
    const std::array<bool, 4> untouched_analysis_mask{
        true, false, true, false};
    REQUIRE(device->set_blend_enabled(sparse_output_location, true));
    REQUIRE(device->set_color_write_mask(
        sparse_output_location, sparse_analysis_mask));
    REQUIRE(device->set_blend_enabled(diagnostic_output_location, true));
    REQUIRE(device->set_color_write_mask(
        diagnostic_output_location, diagnostic_analysis_mask));
    REQUIRE(device->set_blend_enabled(untouched_location, true));
    REQUIRE(device->set_color_write_mask(
        untouched_location, untouched_analysis_mask));

    auto sparse_capture = sparse_renderer->render_analysis(
        *device, mesh, *gpu_mesh, {32, 32},capture_state());
    INFO((sparse_capture
        ? std::string{}
        : describe(sparse_capture.error())));
    REQUIRE(sparse_capture);
    REQUIRE(sparse_renderer->analysis_source());
    REQUIRE(sparse_renderer->analysis_source()->analysis);
    CHECK(sparse_renderer->analysis_source()
              ->analysis->surface_key_location == 1);
    CHECK(sparse_capture->surface_at({16, 16}).has_value());

    const auto check_indexed_color_state = [&](
        std::uint32_t location,
        const std::array<bool, 4>& expected_mask) {
        CHECK(is_enabled_indexed(gl_blend, location) != 0);
        std::array<std::uint8_t, 4> actual_mask{};
        get_boolean_indexed(
            gl_color_writemask, location, actual_mask.data());
        CHECK(actual_mask == std::array<std::uint8_t, 4>{
            static_cast<std::uint8_t>(expected_mask[0]),
            static_cast<std::uint8_t>(expected_mask[1]),
            static_cast<std::uint8_t>(expected_mask[2]),
            static_cast<std::uint8_t>(expected_mask[3]),
        });
    };
    check_indexed_color_state(
        sparse_output_location, sparse_analysis_mask);
    check_indexed_color_state(
        diagnostic_output_location, diagnostic_analysis_mask);
    check_indexed_color_state(
        untouched_location, untouched_analysis_mask);

    const std::array<bool, 4> sparse_observation_mask{
        true, false, false, true};
    const std::array<bool, 4> diagnostic_observation_mask{
        true, true, false, false};
    const std::array<bool, 4> untouched_observation_mask{
        false, true, true, false};
    REQUIRE(device->set_color_write_mask(
        sparse_output_location, sparse_observation_mask));
    REQUIRE(device->set_color_write_mask(
        diagnostic_output_location, diagnostic_observation_mask));
    REQUIRE(device->set_color_write_mask(
        untouched_location, untouched_observation_mask));
    auto sparse_evidence = sparse_renderer->capture(
        *device,
        mesh,
        *gpu_mesh,
        vng::render::RenderView::without_camera({32, 32}),capture_state(),
        vng::analysis::CaptureRequest::standard()
            .observe(ObservedTint{}));
    INFO((sparse_evidence
        ? std::string{}
        : describe(sparse_evidence.error())));
    REQUIRE(sparse_evidence);
    REQUIRE(sparse_evidence->observation(ObservedTint{}) != nullptr);
    check_indexed_color_state(
        sparse_output_location, sparse_observation_mask);
    check_indexed_color_state(
        diagnostic_output_location, diagnostic_observation_mask);
    check_indexed_color_state(
        untouched_location, untouched_observation_mask);

    // Avoid passing deliberately hostile indexed state to unrelated checks.
    for (const auto location : {
             sparse_output_location,
             diagnostic_output_location,
             untouched_location}) {
        REQUIRE(device->set_blend_enabled(location, false));
        REQUIRE(device->set_color_write_mask(
            location, {true, true, true, true}));
    }

    {
        INFO("selective observations become typed evidence channels");
        namespace a = vng::analysis;
        (void)device->take_debug_messages();
        REQUIRE(device->debug_marker("pre-capture diagnostic sentinel"));
        const auto view = vng::render::RenderView::without_camera({32, 32});
        auto request = a::CaptureRequest::diagnostic()
            .observe(ObservedTint{})
            .observe<ObservedCounter>();
        auto evidence = renderer->capture(
            *device,
            mesh,
            *gpu_mesh,
            view,capture_state(),
            std::move(request),
            vng::render::AnalysisOptions{
                .label = "observed triangle",
            });
        INFO((evidence ? std::string{} : describe(evidence.error())));
        REQUIRE(evidence);

        CHECK(evidence->metadata().label == "observed triangle");
        CHECK(evidence->metadata().renderer
              == "vng::render::OpenGLProgramRuntime");
        REQUIRE(evidence->metadata().invocation.has_value());
        CHECK(evidence->metadata().invocation->complete());
        CHECK_FALSE(evidence->metadata().camera.has_value());
        REQUIRE(evidence->metadata().shader.has_value());
        CHECK(evidence->metadata().shader->program_name.find(
                  "analysis_renderer_fragment") != std::string::npos);
        CHECK(evidence->metadata().shader->fragment_ir.find("observe")
              != std::string::npos);
        CHECK(evidence->summary().covered_pixel_count != 0);

        REQUIRE(evidence->request().observations().size() == 2);
        const auto tint_name = std::string{"shader/"}
            + evidence->request().observations()[0].semantic_name;
        const auto counter_name = std::string{"shader/"}
            + evidence->request().observations()[1].semantic_name;
        const auto* tint_channel = evidence->channel(tint_name);
        const auto* counter_channel = evidence->channel(counter_name);
        REQUIRE(tint_channel != nullptr);
        REQUIRE(counter_channel != nullptr);
        REQUIRE(evidence->observation(ObservedTint{}) != nullptr);
        REQUIRE(evidence->observation<ObservedCounter>() != nullptr);
        CHECK(tint_channel->value_kind() == a::EvidenceValueKind::vec3);
        CHECK(counter_channel->value_kind() == a::EvidenceValueKind::u32);

        const auto* tint_image = tint_channel->get_if<vng::Vec3>();
        const auto* counter_image = counter_channel->get_if<vng::u32>();
        REQUIRE(tint_image != nullptr);
        REQUIRE(counter_image != nullptr);
        const auto tint = tint_image->at({16, 16});
        CHECK(std::abs(tint.x - 1.125F) < 0.002F);
        CHECK(std::abs(tint.y - 0.25F) < 0.002F);
        CHECK(std::abs(tint.z - 0.375F) < 0.002F);
        CHECK(counter_image->at({16, 16}) == 37U);
        CHECK(counter_image->at({0, 31}) == 0U);

        auto inspected = evidence->inspect({16, 16});
        REQUIRE(inspected);
        const auto* inspected_tint = inspected->find(tint_name);
        const auto* inspected_counter = inspected->find(counter_name);
        REQUIRE(inspected_tint != nullptr);
        REQUIRE(inspected_counter != nullptr);
        REQUIRE(std::holds_alternative<vng::Vec3>(*inspected_tint));
        REQUIRE(std::holds_alternative<vng::u32>(*inspected_counter));
        CHECK(std::get<vng::u32>(*inspected_counter) == 37U);
        REQUIRE(inspected->observation(ObservedTint{}) != nullptr);
        REQUIRE(inspected->observation<ObservedCounter>() != nullptr);
        CHECK(inspected->observation<ObservedCounter>()[0] == 37U);

        const auto* normal_fragment = evidence->backend_artifact(
            "shader/normal/fragment.glsl");
        const auto* observed_fragment = evidence->backend_artifact(
            "shader/observation-0/fragment.glsl");
        const auto* observed_semantic = evidence->backend_artifact(
            "shader/observation-0/semantic.txt");
        const auto* pipeline_state = evidence->backend_artifact(
            "raster/state.txt");
        REQUIRE(normal_fragment != nullptr);
        REQUIRE(observed_fragment != nullptr);
        REQUIRE(observed_semantic != nullptr);
        REQUIRE(pipeline_state != nullptr);
        CHECK(normal_fragment->text.find("ObservedTint")
              == std::string::npos);
        CHECK(observed_fragment->text.find("layout(location")
              != std::string::npos);
        CHECK(observed_semantic->text.find("ObservedTint")
              != std::string::npos);
        CHECK(pipeline_state->text.find("raster.variant=production")
              != std::string::npos);

        const auto* captured_debug = evidence->backend_artifact(
            "backend/debug-messages.tsv");
        CHECK((captured_debug == nullptr
               || captured_debug->text.find("pre-capture diagnostic sentinel")
                   == std::string::npos));
        const auto global_debug = device->take_debug_messages();
        CHECK(std::ranges::any_of(
            global_debug,
            [](const vng::opengl::DebugMessage& message) {
                return message.message == "pre-capture diagnostic sentinel";
            }));

        const auto raster_property = std::ranges::find(
            evidence->metadata().properties,
            std::string_view{"raster.variant"},
            &a::MetadataEntry::key);
        REQUIRE(raster_property != evidence->metadata().properties.end());
        CHECK(raster_property->value == "production");
    }

    {
        INFO("diagnostic sweep varies one raster decision at a time");
        namespace a = vng::analysis;
        auto depth_rejected_renderer = vng::render::OpenGLProgramRuntime::create(
            *device,
            *shader_program);
        INFO((depth_rejected_renderer
            ? std::string{}
            : describe(depth_rejected_renderer.error())));
        REQUIRE(depth_rejected_renderer);

        const auto view = vng::render::RenderView::without_camera({32, 32});
        auto sweep = depth_rejected_renderer->diagnose(
            *device,
            mesh,
            *gpu_mesh,
            view,capture_state(vng::opengl::GraphicsStateSnapshot{
                .depth = {
                    .test = true,
                    .write = true,
                    .compare = vng::render::DepthCompare::never,
                },
            }),
            a::CaptureRequest::diagnostic(),
            vng::render::AnalysisOptions{
                .label = "depth rejection sweep",
            });
        INFO((sweep ? std::string{} : describe(sweep.error())));
        REQUIRE(sweep);
        REQUIRE(sweep->variants().size() == 3);

        const auto* production = sweep->find(
            a::DiagnosticVariant::production);
        const auto* no_cull = sweep->find(
            a::DiagnosticVariant::cull_disabled);
        const auto* depth_always = sweep->find(
            a::DiagnosticVariant::depth_always);
        REQUIRE(production != nullptr);
        REQUIRE(no_cull != nullptr);
        REQUIRE(depth_always != nullptr);
        CHECK(production->summary().covered_pixel_count == 0);
        CHECK(no_cull->summary().covered_pixel_count == 0);
        CHECK(depth_always->summary().covered_pixel_count != 0);
        CHECK(production->metadata().label == "depth rejection sweep");
        CHECK(no_cull->metadata().label == "depth rejection sweep");
        CHECK(depth_always->metadata().label == "depth rejection sweep");
        REQUIRE(production->metadata().invocation.has_value());
        REQUIRE(no_cull->metadata().invocation.has_value());
        REQUIRE(depth_always->metadata().invocation.has_value());
        CHECK(*production->metadata().invocation
              == *no_cull->metadata().invocation);
        CHECK(*production->metadata().invocation
              == *depth_always->metadata().invocation);

        const auto finding = std::ranges::find(
            sweep->findings(),
            a::FindingCode::likely_depth_rejection,
            &a::DiagnosticFinding::code);
        REQUIRE(finding != sweep->findings().end());
        CHECK(finding->confidence == a::FindingConfidence::strong);

        const auto variant_name = [](const a::FrameEvidence& frame) {
            const auto property = std::ranges::find(
                frame.metadata().properties,
                std::string_view{"raster.variant"},
                &a::MetadataEntry::key);
            return property == frame.metadata().properties.end()
                ? std::string{}
                : property->value;
        };
        CHECK(variant_name(*production) == "production");
        CHECK(variant_name(*no_cull) == "cull_disabled");
        CHECK(variant_name(*depth_always) == "depth_always");

        const auto property_value = [](const a::FrameEvidence& frame,
                                       std::string_view key) {
            const auto property = std::ranges::find(
                frame.metadata().properties,
                key,
                &a::MetadataEntry::key);
            return property == frame.metadata().properties.end()
                ? std::string{}
                : property->value;
        };
        CHECK(property_value(
                  *depth_always, "raster.production.depth.compare")
              == "never");
        CHECK(property_value(
                  *production, "raster.effective.depth.compare")
              == "never");
        CHECK(property_value(
                  *depth_always, "raster.effective.depth.compare")
              == "always");
    }

    {
        INFO("cull-disabled diagnosis preserves clockwise front-face classification");
        auto facing_fragment = vng::shader::fragment<
            FragmentInputs,
            FragmentOutputs>(
            "analysis_clockwise_front_facing_fragment",
            [](auto& stage) {
                const auto color = vng::dsl::select(
                    stage.front_facing(),
                    vng::Vec4{0.0F, 1.0F, 0.0F, 1.0F},
                    vng::Vec4{1.0F, 0.0F, 0.0F, 1.0F});
                return stage.output(
                    vng::dsl::field<vng::shader::Color<0>>(color));
            });
        REQUIRE(facing_fragment);
        auto facing_vertex = vng::shader::vertex<
            VertexInputs,
            VertexOutputs>(
            "analysis_clockwise_front_facing_vertex",
            [](auto& stage) {
                return stage.output(
                    vng::dsl::field<vng::shader::ClipPosition>(
                        vng::dsl::vec4(
                            stage.input(Position{}), 0.0F, 1.0F)),
                    vng::dsl::field<Color>(stage.input(Color{})));
            });
        REQUIRE(facing_vertex);
        auto facing_program = vng::shader::link(
            std::move(*facing_vertex), std::move(*facing_fragment));
        REQUIRE(facing_program);
        auto facing_renderer = vng::render::OpenGLProgramRuntime::create(
            *device,
            *facing_program);
        INFO((facing_renderer
            ? std::string{}
            : describe(facing_renderer.error())));
        REQUIRE(facing_renderer);

        auto sweep = facing_renderer->diagnose(
            *device,
            mesh,
            *gpu_mesh,
            vng::render::RenderView::without_camera({32, 32}),capture_state(vng::opengl::GraphicsStateSnapshot{
                .cull = vng::render::CullMode::back,
                .front_face = vng::render::FrontFace::clockwise,
            }),
            vng::analysis::CaptureRequest::diagnostic());
        INFO((sweep ? std::string{} : describe(sweep.error())));
        REQUIRE(sweep);
        const auto* production = sweep->find(
            vng::analysis::DiagnosticVariant::production);
        const auto* no_cull = sweep->find(
            vng::analysis::DiagnosticVariant::cull_disabled);
        REQUIRE(production != nullptr);
        REQUIRE(no_cull != nullptr);
        CHECK(production->summary().covered_pixel_count == 0);
        const auto clockwise_back_face =
            no_cull->capture().surface_at({16, 16});
        REQUIRE(clockwise_back_face);
        CHECK(clockwise_back_face->color.r > 245);
        CHECK(clockwise_back_face->color.g < 10);
    }

    {
        INFO("requesting an observation absent from the shader is diagnostic");
        const auto view = vng::render::RenderView::without_camera({32, 32});
        auto missing = renderer->capture(
            *device,
            mesh,
            *gpu_mesh,
            view,capture_state(),
            vng::analysis::CaptureRequest::standard()
                .observe(MissingObservation{}));
        REQUIRE_FALSE(missing);
        CHECK(missing.error().code
              == vng::opengl::ErrorCode::invalid_argument);
        CHECK(missing.error().message.find("observation")
              != std::string::npos);
        CHECK(missing.error().message.find("MissingObservation")
              != std::string::npos);
    }

    {
        INFO("capture reflects each invocation's live graphics state, not cached shader state");
        auto frame = vng::render::begin_frame(*device, vng::render::FrameDesc{
            .extent = {32, 32}, .color_encoding = vng::render::ColorEncoding::linear,
            .clear_color = std::nullopt, .clear_depth = std::nullopt});
        REQUIRE(frame);
        auto commands = frame->commands();
        auto graphics = commands.graphics_state();
        const auto view = vng::render::RenderView::without_camera({32, 32});
        REQUIRE(graphics.set(vng::opengl::GraphicsStateSnapshot{}));
        auto visible = renderer->capture(*frame, mesh, *gpu_mesh, view);
        REQUIRE(visible);
        CHECK(visible->summary().covered_pixel_count > 0);
        REQUIRE(graphics.set(vng::render::DepthState{true, true, vng::render::DepthCompare::never}));
        auto rejected = renderer->capture(*frame, mesh, *gpu_mesh, view);
        REQUIRE(rejected);
        CHECK(rejected->summary().covered_pixel_count == 0);
        CHECK(visible->metadata().invocation != rejected->metadata().invocation);
        REQUIRE(graphics.set(vng::render::DepthCompare::greater_equal));
        auto reverse = renderer->capture(*frame, mesh, *gpu_mesh, view);
        REQUIRE(reverse);
        CHECK(reverse->summary().covered_pixel_count > 0);
        REQUIRE(graphics.set(vng::render::BlendMode::additive));
        CHECK_FALSE(renderer->capture(*frame, mesh, *gpu_mesh, view));
        auto srgb = renderer->capture(*device, mesh, *gpu_mesh, view,
            vng::opengl::CaptureState{vng::opengl::GraphicsStateSnapshot{}, vng::render::ColorEncoding::srgb});
        REQUIRE(srgb);
        REQUIRE(srgb->metadata().invocation);
        REQUIRE(visible->metadata().invocation);
        CHECK(srgb->metadata().invocation->renderer_fingerprint == visible->metadata().invocation->renderer_fingerprint);
        CHECK(srgb->metadata().invocation->target_fingerprint != visible->metadata().invocation->target_fingerprint);
        const auto encoding = std::ranges::find(srgb->metadata().properties,
            std::string_view{"raster.production.color_encoding"}, &vng::analysis::MetadataEntry::key);
        REQUIRE(encoding != srgb->metadata().properties.end());
        CHECK(encoding->value == "srgb");
        REQUIRE(graphics.set(vng::render::BlendMode::disabled));
        REQUIRE(graphics.set(vng::opengl::PolygonMode::line));
        CHECK_FALSE(renderer->capture(*frame, mesh, *gpu_mesh, view));
        REQUIRE(graphics.set(vng::opengl::PolygonMode::fill));
        REQUIRE(graphics.set(vng::render::DepthWrite{false}));
        CHECK_FALSE(renderer->capture(*frame, mesh, *gpu_mesh, view));
        CHECK_FALSE(renderer->capture(*frame, mesh, *gpu_mesh,
            vng::render::RenderView::without_camera({16, 16})));
        REQUIRE(frame->end());
        CHECK_FALSE(renderer->capture(*frame, mesh, *gpu_mesh, view));
    }

    {
        INFO("the optional simple mesh renderer is a concrete opengl::Renderer<MeshDraw>");

        vng::gfx::Mesh<PositionOnlyVertex> incompatible_mesh(3);
        incompatible_mesh.add_face(0, 1, 2);
        auto incompatible = vng::render::make_simple_mesh_renderer(
            *device,
            *shader_program,
            std::move(incompatible_mesh));
        REQUIRE_FALSE(incompatible);
        CHECK(incompatible.error().message.find("does not provide")
              != std::string::npos);

        auto routed = vng::render::make_simple_mesh_renderer(
            *device,
            *shader_program,
            mesh);
        INFO((routed ? std::string{} : describe(routed.error())));
        REQUIRE(routed);
        using ConvenienceRenderer =
            std::remove_cvref_t<decltype(*routed)>;
        STATIC_CHECK(vng::render::RendererFor<
            ConvenienceRenderer,
            vng::opengl::Frame>);
        STATIC_CHECK_FALSE(std::is_constructible_v<
            ConvenienceRenderer,
            vng::render::OpenGLProgramRuntime,
            vng::gfx::Mesh<Vertex>,
            vng::opengl::GpuMesh<Vertex>>);

        auto mismatched_renderer = vng::render::make_simple_mesh_renderer(
            *device,
            *shader_program,
            mesh);
        INFO((mismatched_renderer
            ? std::string{}
            : describe(mismatched_renderer.error())));
        REQUIRE(mismatched_renderer);

        const auto view = vng::render::RenderView::without_camera({32, 32});
        const vng::render::MeshDraw draw;

        auto mismatched_frame = vng::render::begin_frame(
            *device,
            vng::render::FrameDesc{
                .extent = {32, 32},
                .color_encoding = vng::render::ColorEncoding::linear,
                .clear_color = std::nullopt,
                .clear_depth = std::nullopt,
            });
        REQUIRE(mismatched_frame);
        const std::array one_draw{draw};
        auto mismatched = mismatched_renderer->render(
            *mismatched_frame, view, std::span{one_draw});
        REQUIRE(mismatched); // Target encoding belongs to the frame, never the program.
        REQUIRE(mismatched_frame->end());

        auto frame = vng::render::begin_frame(
            *device,
            vng::render::FrameDesc{
                .extent = {32, 32},
                .color_encoding = vng::render::ColorEncoding::linear,
                .clear_color = std::array<vng::f32, 4>{
                    0.0F, 0.0F, 0.0F, 1.0F},
                .clear_depth = 1.0F,
            });
        INFO((frame ? std::string{} : describe(frame.error())));
        REQUIRE(frame);

        auto existing_commands = frame->commands();
        const std::array<vng::render::MeshDraw, 0> no_draws{};
        REQUIRE(routed->render(*frame, view, std::span{no_draws}));
        CHECK(existing_commands.active());

        const std::array draw_batch{draw, draw};
        REQUIRE(routed->render(*frame, view, std::span{draw_batch}));
        CHECK(existing_commands.active());

        auto evidence = routed->capture(
            *frame,
            view,
            std::span{one_draw},
            vng::analysis::CaptureRequest::standard()
                .observe(ObservedTint{}));
        INFO((evidence ? std::string{} : describe(evidence.error())));
        REQUIRE(evidence);
        REQUIRE(evidence->observation(ObservedTint{}) != nullptr);
        CHECK(evidence->summary().covered_pixel_count != 0);

        auto sweep = routed->diagnose(
            *frame,
            view,
            std::span{one_draw},
            vng::analysis::CaptureRequest::diagnostic());
        INFO((sweep ? std::string{} : describe(sweep.error())));
        REQUIRE(sweep);
        CHECK(sweep->variants().size() == 3);
        REQUIRE(frame->end());
    }
}

TEST_CASE("renderer supplies a dynamic backend-neutral camera to normal and analysis shaders",
          "[render][opengl][camera]")
{
    auto window = vng::glfw_opengl::create_window(
        vng::window::WindowDesc{
            .width = 32,
            .height = 32,
            .title = "vng hidden camera renderer test",
            .visible = false,
            .resizable = false,
        },
        vng::opengl::ContextDesc{
            .debug = true,
            .samples = 0,
            .default_framebuffer_encoding =
                vng::render::ColorEncoding::linear,
        }, {.vsync = vng::window::VSync::off});
    if (!window) {
        skip_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest(
            "OpenGL context could not be made current: "
            + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe(device.error())));
    REQUIRE(device);

    auto vertex = vng::shader::vertex<CameraVertexInputs, VertexOutputs>(
        "camera_renderer_vertex",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    stage.camera().project(stage.input(Position3D{}))),
                vng::dsl::field<Color>(stage.input(Color{})));
        });
    REQUIRE(vertex);
    auto fragment = vng::shader::fragment<FragmentInputs, FragmentOutputs>(
        "camera_renderer_fragment",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(
                    stage.input(Color{})));
        });
    REQUIRE(fragment);
    auto shader_program = vng::shader::link(
        std::move(*vertex), std::move(*fragment));
    REQUIRE(shader_program);

    auto renderer = vng::render::OpenGLProgramRuntime::create(
        *device, *shader_program);
    INFO((renderer ? std::string{} : describe(renderer.error())));
    REQUIRE(renderer);
    REQUIRE(renderer->normal_source().parameters.size() == 1);
    CHECK(renderer->normal_source().parameters[0].kind
          == vng::shader::ParameterKind::camera_view_projection);

    vng::gfx::Mesh<CameraVertex> mesh(3);
    mesh.vertices()[0].set(Position3D{}, {-0.6F, -0.6F, -2.0F});
    mesh.vertices()[1].set(Position3D{}, {0.6F, -0.6F, -2.0F});
    mesh.vertices()[2].set(Position3D{}, {0.0F, 0.6F, -2.0F});
    for (auto& vertex_value : mesh.vertices()) {
        vertex_value.set(Color{}, {1.0F, 0.25F, 0.05F, 1.0F});
    }
    mesh.add_face(0, 1, 2);
    auto gpu_mesh = vng::opengl::upload_mesh(*device, mesh);
    INFO((gpu_mesh ? std::string{} : describe(gpu_mesh.error())));
    REQUIRE(gpu_mesh);

    vng::gfx::Camera camera;
    constexpr vng::Extent2D extent{32, 32};
    auto frame = vng::render::begin_frame(
        *device,
        vng::render::FrameDesc{
            .extent = extent,
            .color_encoding = vng::render::ColorEncoding::linear,
            .clear_color = std::nullopt,
            .clear_depth = std::nullopt,
        });
    REQUIRE(frame);
    auto commands = frame->commands();
    REQUIRE(commands.run(renderer->production()));

    // A shader that reads stage.camera() cannot accidentally draw before the
    // renderer supplies the current view through the frame command stream.
    auto missing_camera = commands.draw(*gpu_mesh);
    REQUIRE_FALSE(missing_camera);
    CHECK(missing_camera.error().code
          == vng::opengl::ErrorCode::invalid_argument);

    auto view = vng::render::RenderView::create(camera, extent);
    REQUIRE(view);
    REQUIRE(commands.view(*view));
    REQUIRE(commands.draw(*gpu_mesh));
    REQUIRE(frame->end());

    auto centered = renderer->render_analysis(
        *device, mesh, *gpu_mesh, camera, extent,capture_state());
    INFO((centered ? std::string{} : describe(centered.error())));
    REQUIRE(centered);
    CHECK(centered->surface_at({16, 16}).has_value());

    const auto* analysis_source = renderer->analysis_source();
    REQUIRE(analysis_source != nullptr);
    REQUIRE(analysis_source->analysis);
    REQUIRE(analysis_source->parameters.size() == 1);
    CHECK(analysis_source->analysis->first_item_uniform_location
          != analysis_source->parameters[0].location);

    // This updates only the uniform value; the linked shader and renderer are
    // reused. set_position deliberately preserves the viewing direction.
    camera.set_position({3.0F, 0.0F, 0.0F});
    auto moved = renderer->render_analysis(
        *device, mesh, *gpu_mesh, camera, extent,capture_state());
    INFO((moved ? std::string{} : describe(moved.error())));
    REQUIRE(moved);
    CHECK_FALSE(moved->surface_at({16, 16}).has_value());
    CHECK(moved->frame().value == centered->frame().value + 1);
}
