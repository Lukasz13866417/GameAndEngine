#include <vng/gfx/gfx.hpp>
#include <vng/glsl/glsl.hpp>
#include <vng/opengl/glsl_source.hpp>
#include <vng/opengl/opengl.hpp>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>
#include "../support/glfw_opengl.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct MeshPosition : vng::gfx::Semantic<vng::Vec2> {};
struct MeshColor : vng::gfx::Semantic<vng::Vec4> {};

using MeshPositions = vng::gfx::Record<MeshPosition>;
using MeshColors = vng::gfx::Record<
    vng::gfx::as<MeshColor, vng::gfx::unorm8x4>>;
using MeshVertexInputs = vng::shader::VertexInputs<MeshPosition, MeshColor>;
using AlternateMeshVertexInputs = vng::shader::VertexInputs<MeshColor, MeshPosition>;
using MeshVertexOutputs = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<MeshColor>>;
using MeshFragmentInputs = vng::shader::FragmentInputs<
    vng::shader::smooth<MeshColor>>;
using MeshFragmentOutputs = vng::shader::FragmentOutputs<
    vng::shader::Color<0>>;

[[noreturn]] void skip_gpu_mesh_ctest(const std::string& reason)
{
    std::cerr << "OpenGL test skipped: " << reason << '\n';
    std::exit(77);
}

[[nodiscard]] std::string describe_gpu_mesh_error(
    const vng::opengl::Diagnostic& diagnostic)
{
    std::string result = diagnostic.message;
    if (!diagnostic.driver_log.empty()) {
        result += "\ndriver log:\n" + diagnostic.driver_log;
    }
    if (!diagnostic.generated_source.empty()) {
        result += "\ngenerated source:\n" + diagnostic.generated_source;
    }
    return result;
}

[[nodiscard]] bool red_pixel(
    const std::vector<std::byte>& pixels,
    std::size_t width,
    std::size_t x,
    std::size_t y)
{
    const auto offset = (y * width + x) * 4U;
    return std::to_integer<unsigned>(pixels[offset]) > 245U
        && std::to_integer<unsigned>(pixels[offset + 1]) < 10U
        && std::to_integer<unsigned>(pixels[offset + 2]) < 10U
        && std::to_integer<unsigned>(pixels[offset + 3]) > 245U;
}

} // namespace

TEST_CASE("GpuMesh uploads split semantic streams and draws indexed triangles",
          "[opengl][integration][gpu-mesh]")
{
    auto window = vng::test::create_hidden_opengl_window(
        32, 32, "vng hidden GpuMesh test");
    if (!window) {
        skip_gpu_mesh_ctest("OpenGL context unavailable: " + window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_gpu_mesh_ctest(
            "OpenGL context could not be made current: " + access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    INFO((device ? std::string{} : describe_gpu_mesh_error(device.error())));
    REQUIRE(device.has_value());

    auto vertex_stage = vng::shader::vertex<MeshVertexInputs, MeshVertexOutputs>(
        "gpu_mesh_vertex",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::ClipPosition>(
                    vng::dsl::vec4(stage.input(MeshPosition{}), 0.0F, 1.0F)),
                vng::dsl::field<MeshColor>(stage.input(MeshColor{})));
        });
    REQUIRE(vertex_stage.has_value());
    auto fragment_stage = vng::shader::fragment<
        MeshFragmentInputs,
        MeshFragmentOutputs>(
        "gpu_mesh_fragment",
        [](auto& stage) {
            return stage.output(
                vng::dsl::field<vng::shader::Color<0>>(
                    stage.input(MeshColor{})));
        });
    REQUIRE(fragment_stage.has_value());
    auto shader_program = vng::shader::link(
        std::move(*vertex_stage), std::move(*fragment_stage));
    REQUIRE(shader_program.has_value());
    auto generated = vng::glsl::emit(*shader_program);
    REQUIRE(generated.has_value());

    auto vertex_shader = vng::opengl::Shader::compile(
        *device, vng::opengl::from_glsl(generated->vertex));
    INFO((vertex_shader
        ? std::string{}
        : describe_gpu_mesh_error(vertex_shader.error())));
    REQUIRE(vertex_shader.has_value());
    auto fragment_shader = vng::opengl::Shader::compile(
        *device, vng::opengl::from_glsl(generated->fragment));
    INFO((fragment_shader
        ? std::string{}
        : describe_gpu_mesh_error(fragment_shader.error())));
    REQUIRE(fragment_shader.has_value());
    auto program = vng::opengl::Program::link_graphics(
        *device, *vertex_shader, *fragment_shader);
    INFO((program ? std::string{} : describe_gpu_mesh_error(program.error())));
    REQUIRE(program.has_value());
    auto compiled_program = vng::render::compile_program(*device, *shader_program);
    INFO((compiled_program
        ? std::string{}
        : describe_gpu_mesh_error(compiled_program.error())));
    REQUIRE(compiled_program.has_value());

    // The CPU mesh deliberately stores color before position. Shader input
    // order, stream order, and physical color encoding are independent.
    vng::gfx::Mesh<MeshColors, MeshPositions> mesh(3);
    mesh.vertices<MeshPositions>()[0].set(MeshPosition{}, {-0.6F, -0.6F});
    mesh.vertices<MeshPositions>()[1].set(MeshPosition{}, {0.6F, -0.6F});
    mesh.vertices<MeshPositions>()[2].set(MeshPosition{}, {0.0F, 0.6F});
    for (auto& color : mesh.vertices<MeshColors>()) {
        color.set(MeshColor{}, {1.0F, 0.0F, 0.0F, 1.0F});
    }
    mesh.add_face(0, 1, 2);

    auto gpu_mesh = vng::opengl::upload_mesh(*device, mesh);
    INFO((gpu_mesh ? std::string{} : describe_gpu_mesh_error(gpu_mesh.error())));
    REQUIRE(gpu_mesh.has_value());
    CHECK(gpu_mesh->vertex_count() == 3);
    CHECK(gpu_mesh->face_count() == 1);
    CHECK(gpu_mesh->index_count() == 3);
    CHECK(gpu_mesh->topology_fingerprint() == mesh.topology_fingerprint());
    CHECK(gpu_mesh->vertex_stream_count() == 2);
    CHECK(gpu_mesh->cached_vertex_input_count() == 0);

    auto color = vng::opengl::Renderbuffer::create(*device, 32, 32);
    REQUIRE(color.has_value());
    auto framebuffer = vng::opengl::Framebuffer::create(*device);
    REQUIRE(framebuffer.has_value());
    REQUIRE(framebuffer->attach_color(0, *color).has_value());
    REQUIRE(framebuffer->check_complete().has_value());
    REQUIRE(framebuffer->bind().has_value());
    REQUIRE(framebuffer->clear_color(0, {0.0F, 0.0F, 0.0F, 1.0F}).has_value());
    REQUIRE(device->viewport(0, 0, 32, 32).has_value());
    auto frame = vng::render::begin_frame(
        *device, *framebuffer, vng::render::FrameDesc{
            .extent = {32, 32},
            .color_encoding = vng::render::ColorEncoding::linear,
            .clear_color = std::nullopt, .clear_depth = std::nullopt,
        });
    REQUIRE(frame);
    auto commands = frame->commands();
    auto graphics = commands.graphics_state();
    REQUIRE(graphics.set(vng::render::DepthTest{false}));
    REQUIRE(commands.run(*compiled_program));
    auto drawn = commands.draw(*gpu_mesh);
    INFO((drawn ? std::string{} : describe_gpu_mesh_error(drawn.error())));
    REQUIRE(drawn.has_value());
    CHECK(gpu_mesh->cached_vertex_input_count() == 1);
    REQUIRE(device->finish().has_value());

    auto pixels = framebuffer->read_rgba8(0, 0, 0, 32, 32);
    REQUIRE(pixels.has_value());
    CHECK(red_pixel(*pixels, 32, 16, 16));
    REQUIRE(frame->end());

    // A second valid location order gets its own VAO; returning to the first
    // contract reuses the existing entry.
    REQUIRE(gpu_mesh->prepare_vertex_input<AlternateMeshVertexInputs>(*device)
        .has_value());
    CHECK(gpu_mesh->cached_vertex_input_count() == 2);
    REQUIRE(gpu_mesh->bind(*device, *program).has_value());
    CHECK(gpu_mesh->cached_vertex_input_count() == 2);

    auto moved_gpu_mesh = std::move(*gpu_mesh);
    CHECK(moved_gpu_mesh.topology_fingerprint() == mesh.topology_fingerprint());
    CHECK(moved_gpu_mesh.cached_vertex_input_count() == 2);
    REQUIRE(moved_gpu_mesh.draw(*device, *program).has_value());
    CHECK(moved_gpu_mesh.cached_vertex_input_count() == 2);

    auto second_device_facade = vng::opengl::Device::create(*access);
    REQUIRE(second_device_facade.has_value());
    REQUIRE(moved_gpu_mesh.draw(*second_device_facade, *program, 1).has_value());

    auto assignment_target = vng::opengl::upload_mesh(*device, mesh);
    REQUIRE(assignment_target.has_value());
    REQUIRE(assignment_target->draw(*device, *program, 0).has_value());
    *assignment_target = std::move(moved_gpu_mesh);
    CHECK(assignment_target->cached_vertex_input_count() == 2);
    REQUIRE(assignment_target->draw(*device, *program).has_value());

    // Editing generated text turns it into raw GLSL and invalidates the
    // sealed semantic contract, even when the textual edit remains legal.
    auto raw_vertex_source = vng::opengl::from_glsl(generated->vertex);
    raw_vertex_source.text += '\n';
    auto raw_vertex_shader = vng::opengl::Shader::compile(
        *device, std::move(raw_vertex_source));
    REQUIRE(raw_vertex_shader.has_value());
    auto raw_program = vng::opengl::Program::link_graphics(
        *device, *raw_vertex_shader, *fragment_shader);
    REQUIRE(raw_program.has_value());
    auto rejected_raw_program = assignment_target->draw(*device, *raw_program);
    REQUIRE_FALSE(rejected_raw_program.has_value());
    CHECK(rejected_raw_program.error().message.find("raw GLSL")
        != std::string::npos);

    // The ordinary program-driven API cannot silently apply the wrong
    // contract: a mesh missing the program's color semantic is rejected.
    vng::gfx::Mesh<MeshPositions> positions_only(3);
    positions_only.vertices()[0].set(MeshPosition{}, {-0.6F, -0.6F});
    positions_only.vertices()[1].set(MeshPosition{}, {0.6F, -0.6F});
    positions_only.vertices()[2].set(MeshPosition{}, {0.0F, 0.6F});
    positions_only.add_face(0, 1, 2);
    auto incomplete_gpu_mesh = vng::opengl::upload_mesh(*device, positions_only);
    REQUIRE(incomplete_gpu_mesh.has_value());
    auto rejected_contract = incomplete_gpu_mesh->draw(*device, *program);
    REQUIRE_FALSE(rejected_contract.has_value());
    CHECK(rejected_contract.error().code
        == vng::opengl::ErrorCode::invalid_argument);
    CHECK(rejected_contract.error().message.find("MeshColor")
        != std::string::npos);

    vng::gfx::Mesh<MeshColors, MeshPositions> mismatched(3);
    mismatched.vertices<MeshPositions>().resize(2);
    auto rejected_mismatch = vng::opengl::upload_mesh(*device, mismatched);
    REQUIRE_FALSE(rejected_mismatch.has_value());
    CHECK(rejected_mismatch.error().code == vng::opengl::ErrorCode::invalid_argument);
    CHECK(rejected_mismatch.error().message.find("vertex stream") != std::string::npos);

    vng::gfx::Mesh<MeshColors, MeshPositions> bad_face(3);
    bad_face.add_face(0, 1, 3);
    auto rejected_face = vng::opengl::upload_mesh(*device, bad_face);
    REQUIRE_FALSE(rejected_face.has_value());
    CHECK(rejected_face.error().code == vng::opengl::ErrorCode::invalid_argument);
    CHECK(rejected_face.error().message.find("face 0") != std::string::npos);

    // Empty geometry remains a useful asset state. It has complete GL
    // ownership and a valid VAO but emits a zero-count draw.
    vng::gfx::Mesh<MeshColors, MeshPositions> empty;
    auto empty_gpu_mesh = vng::opengl::upload_mesh(*device, empty);
    INFO((empty_gpu_mesh
        ? std::string{}
        : describe_gpu_mesh_error(empty_gpu_mesh.error())));
    REQUIRE(empty_gpu_mesh.has_value());
    REQUIRE(empty_gpu_mesh->draw(*device, *program).has_value());
    CHECK(empty_gpu_mesh->vertex_count() == 0);
    CHECK(empty_gpu_mesh->index_count() == 0);
}
