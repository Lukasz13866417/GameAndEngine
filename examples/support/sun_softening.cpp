#include "sun_softening.hpp"

#include <vng/gfx/gfx.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/image.hpp>
#include <vng/opengl/render_state_scope.hpp>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>

#include <glad/gl.h>

#include <array>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace example::sun {
namespace {
using namespace vng;
using resources::Result;
using resources::into_result;

struct Position : gfx::Semantic<Vec2> {};
struct UV : gfx::Semantic<Vec2> {};
using Vertex = gfx::Record<Position, UV>;
using Inputs = shader::VertexInputs<Position, UV>;
using Varyings = shader::VertexOutputs<shader::ClipPosition, shader::smooth<UV>>;
using FragmentInputs = shader::FragmentInputs<shader::smooth<UV>>;
using Outputs = shader::FragmentOutputs<shader::Color<0>>;
using Program = opengl::TypedProgram<Vec2>;

resources::Diagnostic invalid(std::string message)
{ return {.code = resources::ErrorCode::invalid_argument, .message = std::move(message)}; }

template<class Function>
Result<void> checked_gl(std::string_view operation, Function&& function)
{
    while (glGetError() != GL_NO_ERROR) {}
    std::forward<Function>(function)();
    const auto error = glGetError();
    if (error == GL_NO_ERROR) return {};
    while (glGetError() != GL_NO_ERROR) {}
    return std::unexpected(resources::to_diagnostic(opengl::Diagnostic{
        .code = opengl::ErrorCode::operation_failed,
        .message = std::string(operation) + " failed with OpenGL error " + std::to_string(error)}));
}

// Borrow the source's sampling policy and texture unit zero for one pass.
// Restoring the 2D target specifically leaves other targets on that unit alone.
class SamplingScope final {
public:
    static Result<SamplingScope> capture(const opengl::Image2D& image)
    {
        SamplingScope scope{image.native_handle()};
        auto captured = checked_gl("capture sun softening sampling state", [&] {
            glGetIntegeri_v(GL_TEXTURE_BINDING_2D, 0, &scope.texture_);
            glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &scope.sampler_);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &scope.active_texture_);
            for (std::size_t i = 0; i < parameters.size(); ++i)
                glGetTextureParameteriv(scope.source_, parameters[i], &scope.values_[i]);
        });
        if (!captured) return std::unexpected(std::move(captured.error()));
        scope.active_ = true;
        if (auto configured = checked_gl("configure sun softening sampling", [&] {
                glBindSampler(0, 0);
                glTextureParameteri(scope.source_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTextureParameteri(scope.source_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTextureParameteri(scope.source_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTextureParameteri(scope.source_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTextureParameteri(scope.source_, GL_TEXTURE_BASE_LEVEL, 0);
                glTextureParameteri(scope.source_, GL_TEXTURE_MAX_LEVEL, 0);
            }); !configured) return std::unexpected(std::move(configured.error()));
        return scope;
    }
    SamplingScope(SamplingScope&& other) noexcept
        : source_(other.source_), texture_(other.texture_), sampler_(other.sampler_),
          active_texture_(other.active_texture_), values_(other.values_),
          active_(std::exchange(other.active_, false)) {}
    ~SamplingScope() { if (active_) restore_native(); }
    Result<void> restore()
    {
        if (!active_) return {};
        active_ = false;
        return checked_gl("restore sun softening sampling state", [&] { restore_native(); });
    }
private:
    explicit SamplingScope(GLuint source) : source_(source) {}
    void restore_native() const
    {
        for (std::size_t i = 0; i < parameters.size(); ++i)
            glTextureParameteri(source_, parameters[i], values_[i]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_));
        glBindSampler(0, static_cast<GLuint>(sampler_));
        glActiveTexture(static_cast<GLenum>(active_texture_));
    }
    static constexpr std::array<GLenum, 6> parameters{
        GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S,
        GL_TEXTURE_WRAP_T, GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_MAX_LEVEL};
    GLuint source_{};
    GLint texture_{}, sampler_{}, active_texture_{GL_TEXTURE0};
    std::array<GLint, parameters.size()> values_{};
    bool active_{};
};

Result<Program> make_program(opengl::Device& device)
{
    auto vertex = shader::vertex<Inputs, Varyings>("sun_softening_vertex", [](auto& s) {
        return s.output(dsl::field<shader::ClipPosition>(dsl::vec4(s.input(Position{}), 0.0F, 1.0F)),
                        dsl::field(UV{}, s.input(UV{})));
    });
    if (!vertex) return std::unexpected(resources::to_diagnostic(std::move(vertex.error())));
    auto fragment = shader::fragment<FragmentInputs, Outputs>("sun_softening_fragment",
        [](auto& s, dsl::Float2 texel) {
            const auto uv = s.input(UV{});
            const auto x = dsl::vec2(texel.x(), 0.0F);
            const auto y = dsl::vec2(0.0F, texel.y());
            const auto sample = [&](dsl::Float2 at) { return s.template sample_2d<0>(at); };
            // Truncated, normalized Gaussian with sigma = 0.75 pixels.
            // Applying the same weights to alpha preserves constant RGBA.
            constexpr f32 side = 0.22561011F;
            constexpr f32 middle = 1.0F - 2.0F * side;
            const auto center = sample(uv) * (middle * middle);
            const auto axes = (sample(uv - x) + sample(uv + x)
                + sample(uv - y) + sample(uv + y)) * (middle * side);
            const auto corners = (sample(uv - x - y) + sample(uv + x - y)
                + sample(uv - x + y) + sample(uv + x + y)) * (side * side);
            return s.output(dsl::field<shader::Color<0>>(center + axes + corners));
        });
    if (!fragment) return std::unexpected(resources::to_diagnostic(std::move(fragment.error())));
    auto linked = shader::link(std::move(*vertex), std::move(*fragment));
    if (!linked) return std::unexpected(resources::to_diagnostic(std::move(linked.error())));
    return into_result(render::compile_program(device, *linked));
}
} // namespace

struct SunSoftening::Impl final {
    Program program;
    opengl::GpuMesh<Vertex> triangle;
};

SunSoftening::SunSoftening(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SunSoftening::SunSoftening(SunSoftening&&) noexcept = default;
SunSoftening& SunSoftening::operator=(SunSoftening&&) noexcept = default;
SunSoftening::~SunSoftening() = default;

Result<SunSoftening> SunSoftening::create(opengl::Device& device)
{
    if (auto allowed = into_result(device.require_resource_update("SunSoftening::create")); !allowed)
        return std::unexpected(std::move(allowed.error()));
    auto program = make_program(device);
    if (!program) return std::unexpected(std::move(program.error()));
    gfx::Mesh<Vertex> mesh(3);
    constexpr std::array<Vec2, 3> points{{{-1, -1}, {3, -1}, {-1, 3}}};
    constexpr std::array<Vec2, 3> uvs{{{0, 0}, {2, 0}, {0, 2}}};
    for (u32 i = 0; i < points.size(); ++i) {
        mesh.vertices()[i].set(Position{}, points[i]);
        mesh.vertices()[i].set(UV{}, uvs[i]);
    }
    mesh.add_face(0, 1, 2);
    auto triangle = into_result(opengl::upload_mesh(device, mesh));
    if (!triangle) return std::unexpected(std::move(triangle.error()));
    if (auto ready = into_result(triangle->prepare_vertex_input(device, *program)); !ready)
        return std::unexpected(std::move(ready.error()));
    return SunSoftening{std::make_unique<Impl>(std::move(*program), std::move(*triangle))};
}

Result<void> SunSoftening::apply(opengl::Frame& output, const opengl::Image2D& source)
{
    if (!impl_ || !output.active())
        return std::unexpected(invalid("SunSoftening needs a live effect and active output frame"));
    const auto& device = output.device();
    if (auto current = into_result(device.require_current("SunSoftening::apply")); !current) return current;
    if (!impl_->program.belongs_to(device) || !source.belongs_to(device))
        return std::unexpected(invalid("SunSoftening input, program and output must belong to the same device"));
    const auto extent = output.extent();
    if (extent.empty() || extent != source.extent())
        return std::unexpected(invalid("SunSoftening needs matching nonempty input and output extents"));
    if (output.color_encoding() != render::ColorEncoding::linear
        || (source.format() != gfx::ImageFormat::rgba16f && source.format() != gfx::ImageFormat::rgba32f))
        return std::unexpected(invalid("SunSoftening needs linear floating-point HDR input and output"));
    if (output.uses_image(source))
        return std::unexpected(invalid("SunSoftening input cannot be attached to its output frame"));

    GLint framebuffer{}, maximum_outputs{}, draw_buffer{}, component_type{};
    if (auto queried = checked_gl("validate sun softening output", [&] {
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
            glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maximum_outputs);
            glGetIntegerv(GL_DRAW_BUFFER0, &draw_buffer);
            if (framebuffer != 0 && draw_buffer != GL_NONE)
                glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, static_cast<GLenum>(draw_buffer),
                    GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component_type);
        }); !queried) return queried;
    if (framebuffer == 0 || component_type != GL_FLOAT || maximum_outputs <= 0 || maximum_outputs > 1024)
        return std::unexpected(invalid("SunSoftening output 0 must be a floating-point offscreen attachment"));
    std::vector<u32> outputs(static_cast<std::size_t>(maximum_outputs));
    std::iota(outputs.begin(), outputs.end(), 0U);
    auto state = into_result(opengl::RenderStateScope::capture(device, outputs));
    if (!state) return std::unexpected(std::move(state.error()));
    auto sampling = SamplingScope::capture(source);
    if (!sampling) return std::unexpected(std::move(sampling.error()));
    // Borrow the existing frame context and establish this pass's requirements
    // explicitly. The enclosing scope restores raw state and invalidates its
    // managed binding cache without revoking any parent command handles.
    if (auto set = into_result(device.set_standard_raster_state()); !set) return set;
    if (auto set = into_result(device.set_scissor_enabled(false)); !set) return set;
    if (auto set = into_result(device.set_rasterizer_discard_enabled(false)); !set) return set;
    auto commands = output.render_context();
    const Vec2 texel{1.0F / static_cast<f32>(extent.width), 1.0F / static_cast<f32>(extent.height)};
    if (auto run = into_result(commands.run(impl_->program, texel)); !run) return run;
    auto graphics = commands.graphics_state();
    if (auto set = into_result(graphics.set(render::DepthState{false, false})); !set) return set;
    if (auto set = into_result(graphics.set(render::BlendMode::disabled)); !set) return set;
    if (auto set = into_result(graphics.set(render::CullMode::none)); !set) return set;
    if (auto set = into_result(graphics.set(opengl::PolygonMode::fill)); !set) return set;
    if (auto set = into_result(device.viewport(0, 0, static_cast<i32>(extent.width),
            static_cast<i32>(extent.height))); !set) return set;
    if (auto set = into_result(device.set_framebuffer_srgb_enabled(false)); !set) return set;
    for (auto index : outputs) {
        const bool enabled = index == 0;
        if (auto set = into_result(device.set_color_write_mask(index, {enabled, enabled, enabled, enabled})); !set)
            return set;
    }
    if (auto bound = into_result(source.bind_to_unit(0)); !bound) return bound;
    if (auto drawn = into_result(commands.draw(impl_->triangle)); !drawn) return drawn;
    if (auto restored = sampling->restore(); !restored) return restored;
    return into_result(state->restore());
}

} // namespace example::sun
