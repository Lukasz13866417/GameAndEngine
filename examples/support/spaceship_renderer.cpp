#include "spaceship_renderer.hpp"

#include <vng/opengl/gpu_mesh.hpp>
#include <vng/render_opengl/program_runtime.hpp>
#include <vng/shader/shader.hpp>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace example::spaceflight {
namespace {
using namespace vng;
using resources::Result;
using resources::into_result;

resources::Diagnostic invalid(std::string message)
{
    return {.code = resources::ErrorCode::invalid_argument,
        .message = std::move(message)};
}

using Inputs = shader::VertexInputs<gfx::Position, gfx::Normal, gfx::Color, Emission>;
using Varyings = shader::VertexOutputs<shader::ClipPosition,
    shader::smooth<WorldPosition>, shader::smooth<WorldNormal>,
    shader::smooth<gfx::Color>, shader::smooth<Emission>>;
using FragmentInputs = shader::FragmentInputs<shader::smooth<WorldPosition>,
    shader::smooth<WorldNormal>, shader::smooth<gfx::Color>, shader::smooth<Emission>>;
using Outputs = shader::FragmentOutputs<shader::Color<0>>;
struct Eye : gfx::Semantic<Vec3> {};
struct Light : gfx::Semantic<Vec4> {};
struct LightColor : gfx::Semantic<Vec3> {};
using Lighting = gfx::Record<Eye, Light, LightColor>;
using ShipProgram = shader::TypedGraphicsProgram<Mat4, Lighting>;
using ShipRuntime = render::TypedOpenGLProgramRuntime<Mat4, Lighting>;

constexpr opengl::GraphicsStateSnapshot ship_raster()
{
    return {.depth = {true, true, render::DepthCompare::less}, .cull = render::CullMode::back,
        .front_face = render::FrontFace::counter_clockwise, .blend = render::BlendMode::disabled,
        .polygon = opengl::PolygonMode::fill};
}

Lighting lighting(const ShipDraw& draw, const render::RenderView& view)
{
    Lighting result;
    result.set(Eye{}, view.camera()->position);
    const auto p = draw.light_position.value_or(Vec3{-0.35F, 0.8F, -0.65F});
    result.set(Light{}, {p.x, p.y, p.z, draw.light_position ? 1.0F : 0.0F});
    result.set(LightColor{}, draw.light_color);
    return result;
}

Result<void> validate_light(const ShipDraw& draw)
{
    for (u32 i = 0; i < 3; ++i)
        if (!std::isfinite(draw.light_color[i]) || draw.light_color[i] < 0
            || (draw.light_position && !std::isfinite((*draw.light_position)[i])))
            return std::unexpected(invalid("Ship lighting needs finite position and nonnegative finite color"));
    return {};
}

// Ordinary expression helpers are inlined into the same typed shader DAG.
dsl::Float3 safe_normalize(dsl::Float3 value)
{
    return value / dsl::sqrt(dsl::max(dsl::dot(value, value), 1.0e-12F));
}

Result<ShipProgram> ship_shaders()
{
    auto vertex = shader::vertex<Inputs, Varyings>("ship_vertex", [](auto& stage, dsl::Float4x4 model) {
        const auto world = model * dsl::vec4(stage.input(gfx::Position{}), 1.0F);
        const auto normal = model * dsl::vec4(stage.input(gfx::Normal{}), 0.0F);
        return stage.output(
            dsl::field<shader::ClipPosition>(stage.camera().project(world.xyz())),
            dsl::field<WorldPosition>(world.xyz()),
            dsl::field<WorldNormal>(normal.xyz()),
            dsl::field<gfx::Color>(stage.input(gfx::Color{})),
            dsl::field<Emission>(stage.input(Emission{})));
    });
    if (!vertex) return std::unexpected(resources::to_diagnostic(vertex.error()));
    auto fragment = shader::fragment<FragmentInputs, Outputs>("ship_fragment", [](auto& stage, dsl::Expr<Lighting> lights) {
        const auto world = stage.input(WorldPosition{});
        const auto normal = safe_normalize(stage.input(WorldNormal{}));
        const auto view = safe_normalize(lights.get(Eye{}) - world);
        const auto source = lights.get(Light{});
        const auto sun = safe_normalize(source.xyz() - world * source.w());
        const auto fill = dsl::normalize(stage.constant(Vec3{0.8F, 0.2F, -0.6F}));
        const auto diffuse = dsl::max(dsl::dot(normal, sun), 0.0F);
        const auto blue_fill = dsl::max(dsl::dot(normal, fill), 0.0F);
        const auto half_vector = safe_normalize(sun + view);
        const auto specular = dsl::max(dsl::dot(normal, half_vector), 0.0F);
        const auto specular2 = specular * specular;
        const auto specular4 = specular2 * specular2;
        const auto specular8 = specular4 * specular4;
        const auto specular16 = specular8 * specular8;
        const auto specular32 = specular16 * specular16;
        const auto grazing = 1.0F - dsl::max(dsl::dot(normal, view), 0.0F);
        const auto rim2 = grazing * grazing;
        const auto rim4 = rim2 * rim2;
        const auto color = stage.input(gfx::Color{});
        const auto sky = stage.constant(Vec3{0.17F, 0.23F, 0.34F});
        const auto sunlight = lights.get(LightColor{});
        const auto cold_fill = stage.constant(Vec3{0.11F, 0.28F, 0.6F});
        const auto lit = color.xyz() * (sky + sunlight * diffuse + cold_fill * blue_fill);
        const auto glint = sunlight * (specular32 * 0.75F)
            + stage.constant(Vec3{0.13F, 0.3F, 0.5F}) * (rim4 * 0.45F);
        const auto radiance = dsl::vec4(
            lit + glint + color.xyz() * stage.input(Emission{}), color.w());
        stage.observe(WorldPosition{}, world);
        stage.observe(WorldNormal{}, normal);
        stage.observe(Radiance{}, radiance);
        return stage.output(dsl::field<shader::Color<0>>(radiance));
    });
    if (!fragment) return std::unexpected(resources::to_diagnostic(fragment.error()));
    return into_result(shader::link(std::move(*vertex), std::move(*fragment)));
}

// Resource identity includes vertex attributes and submitted values, not only
// triangle topology. This catches comparisons of different emission/placement.
class Fingerprint final {
public:
    void append(std::span<const std::byte> bytes) noexcept
    {
        for (const auto value : bytes) {
            const auto byte = std::to_integer<u8>(value);
            first_ = (first_ ^ byte) * 0x100000001B3ULL;
            second_ = (second_ + byte + 0x9E3779B97F4A7C15ULL) * 0xBF58476D1CE4E5B9ULL;
            second_ ^= second_ >> 31;
        }
    }
    [[nodiscard]] std::string finish() const
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string text(32, '0');
        for (u32 i = 0; i < 16; ++i) {
            const auto shift = (15U - i) * 4U;
            text[i] = digits[(first_ >> shift) & 15];
            text[16 + i] = digits[(second_ >> shift) & 15];
        }
        return text;
    }
private:
    u64 first_{0xCBF29CE484222325ULL}, second_{0x6A09E667F3BCC909ULL};
};

Result<void> validate_transform(const Mat4& matrix)
{
    for (const auto& column : matrix.columns)
        for (u32 i = 0; i < 4; ++i)
            if (!std::isfinite(column[i]))
                return std::unexpected(invalid("Ship transform must be finite"));
    for (u32 a = 0; a < 3; ++a) {
        if (std::abs(matrix[a].w) > 1.0e-5F)
            return std::unexpected(invalid("Ship transform must be affine"));
        for (u32 b = 0; b < 3; ++b) {
            f32 dot{};
            for (u32 i = 0; i < 3; ++i) dot += matrix[a][i] * matrix[b][i];
            if (std::abs(dot - (a == b ? 1.0F : 0.0F)) > 1.0e-3F)
                return std::unexpected(invalid("Ship transform requires a rigid rotation, without scaling or shear"));
        }
    }
    if (std::abs(matrix[3].w - 1.0F) > 1.0e-5F)
        return std::unexpected(invalid("Ship transform must be affine"));
    const auto& a = matrix[0]; const auto& b = matrix[1]; const auto& c = matrix[2];
    const f32 determinant = a.x * (b.y*c.z - b.z*c.y)
        - b.x * (a.y*c.z - a.z*c.y) + c.x * (a.y*b.z - a.z*b.y);
    if (determinant < 0.0F)
        return std::unexpected(invalid("Ship transform must not reverse face winding"));
    return {};
}
} // namespace

struct ShipRenderer::Impl final {
    ShipRuntime shaders;
    Mesh source;
    opengl::GpuMesh<Vertex> gpu;
    Fingerprint static_fingerprint;

    Impl(ShipRuntime runtime, Mesh mesh, opengl::GpuMesh<Vertex> uploaded)
        : shaders(std::move(runtime)), source(std::move(mesh)),
          gpu(std::move(uploaded))
    { static_fingerprint.append(source.vertices().bytes()); }

    Result<void> validate_frame(const opengl::Frame& frame, const render::RenderView& view) const
    {
        if (!frame.active() || frame.extent().empty() || frame.extent() != view.extent() || !view.camera())
            return std::unexpected(invalid("Ship rendering needs an active frame and matching camera view"));
        if (!shaders.production().untyped().belongs_to(frame.device()))
            return std::unexpected(invalid("Ship renderer belongs to another OpenGL context"));
        return into_result(frame.device().require_current("ShipRenderer"));
    }
};

ShipRenderer::ShipRenderer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ShipRenderer::ShipRenderer(ShipRenderer&&) noexcept = default;
ShipRenderer& ShipRenderer::operator=(ShipRenderer&&) noexcept = default;
ShipRenderer::~ShipRenderer() = default;

Result<ShipRenderer> ShipRenderer::create(opengl::Device& device, Mesh mesh)
{
    if (auto ready = device.require_resource_update("ShipRenderer::create"); !ready)
        return std::unexpected(resources::to_diagnostic(std::move(ready.error())));
    if (mesh.empty() || mesh.face_count() == 0)
        return std::unexpected(invalid("Ship mesh must contain triangles"));
    if (auto valid = mesh.validate(); !valid)
        return std::unexpected(resources::to_diagnostic(std::move(valid.error())));
    for (const auto& vertex : mesh.vertices()) {
        const auto p = vertex.get(gfx::Position{});
        const auto n = vertex.get(gfx::Normal{});
        const auto c = vertex.get(gfx::Color{});
        const auto emission = vertex.get(Emission{});
        const auto finite3 = [](Vec3 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };
        if (!finite3(p) || !finite3(n) || !finite3({c.x,c.y,c.z}) || !std::isfinite(c.w)
            || !std::isfinite(emission) || emission < 0.0F
            || n.x*n.x+n.y*n.y+n.z*n.z < 1.0e-12F)
            return std::unexpected(invalid("Ship vertex requires finite attributes, nonzero normal, and nonnegative emission"));
    }
    auto program = ship_shaders();
    if (!program) return std::unexpected(std::move(program.error()));
    auto shaders = ShipRuntime::create(device, std::move(*program));
    if (!shaders) return std::unexpected(resources::to_diagnostic(std::move(shaders.error())));
    auto gpu = opengl::upload_mesh(device, mesh);
    if (!gpu) return std::unexpected(resources::to_diagnostic(std::move(gpu.error())));
    if (auto prepared = gpu->prepare_vertex_input(device, shaders->production()); !prepared)
        return std::unexpected(resources::to_diagnostic(std::move(prepared.error())));
    return ShipRenderer{std::make_unique<Impl>(std::move(*shaders), std::move(mesh),
        std::move(*gpu))};
}

Result<void> ShipRenderer::render(opengl::Frame& frame, const render::RenderView& view,
    const ShipDraw& draw)
{ return render(frame, view, std::span{&draw, 1}); }

Result<void> ShipRenderer::render(opengl::Frame& frame, const render::RenderView& view,
    std::span<const ShipDraw> draws)
{
    if (!impl_) return std::unexpected(invalid("Ship renderer is empty"));
    if (auto valid = impl_->validate_frame(frame, view); !valid) return valid;
    for (const auto& draw : draws) {
        if (auto valid = validate_transform(draw.transform); !valid) return valid;
        if (auto valid = validate_light(draw); !valid) return valid;
    }
    if (draws.empty()) return {};
    auto context = frame.render_context();
    auto graphics = context.graphics_state();
    if (auto changed = graphics.set(ship_raster()); !changed) return into_result(std::move(changed));
    for (const auto& draw : draws) {
        if (auto selected = context.run(impl_->shaders.production(), draw.transform, lighting(draw, view)); !selected)
            return into_result(std::move(selected));
        if (auto uploaded = context.view(view); !uploaded) return into_result(std::move(uploaded));
        if (auto rendered = context.draw(impl_->gpu); !rendered) return into_result(std::move(rendered));
    }
    return {};
}

Result<analysis::DiagnosticSweep> ShipRenderer::diagnose(opengl::Frame& frame,
    const render::RenderView& view, const ShipDraw& draw, const analysis::CaptureRequest& request)
{
    if (!impl_) return std::unexpected(invalid("Ship renderer is empty"));
    if (auto valid = impl_->validate_frame(frame, view); !valid)
        return std::unexpected(std::move(valid.error()));
    if (auto valid = validate_transform(draw.transform); !valid)
        return std::unexpected(std::move(valid.error()));
    if (auto valid = validate_light(draw); !valid)
        return std::unexpected(std::move(valid.error()));
    const auto lights = lighting(draw, view);
    if (auto supplied = impl_->shaders.set_arguments(draw.transform, lights); !supplied)
        return std::unexpected(resources::to_diagnostic(std::move(supplied.error())));
    render::AnalysisOptions options;
    options.label = "spaceship";
    options.provenance.object_to_world = draw.transform;
    auto fingerprint = impl_->static_fingerprint;
    fingerprint.append(std::as_bytes(std::span{&draw.transform, 1}));
    fingerprint.append(std::as_bytes(std::span{&view.camera()->position, 1}));
    fingerprint.append(lights.bytes());
    options.resource_fingerprint = fingerprint.finish();
    auto sweep = impl_->shaders.diagnose(frame.device(), impl_->source, impl_->gpu,
        view, opengl::CaptureState{ship_raster(), frame.color_encoding()}, request, std::move(options));
    if (!sweep) return std::unexpected(resources::to_diagnostic(std::move(sweep.error())));
    return std::move(*sweep);
}

const Mesh& ShipRenderer::source_mesh() const
{
    if (!impl_) throw std::logic_error("Ship renderer is empty");
    return impl_->source;
}

static_assert(render::RendererFor<ShipRenderer, opengl::Frame>);
} // namespace example::spaceflight
