#include "sun_renderer.hpp"
#include "sun_shaders.hpp"

#include <vng/opengl/gpu_mesh.hpp>
#include <vng/opengl/image.hpp>
#include <vng/render_opengl/program_runtime.hpp>
#include <vng/render/program.hpp>
#include <glad/gl.h>

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace example::sun {
namespace {
using namespace vng;
using resources::Result;
using resources::into_result;
using Runtime = render::TypedOpenGLProgramRuntime<shaders::Parameters, Mat3>;
using Program = opengl::TypedProgram<shaders::Parameters, Mat3>;

constexpr opengl::GraphicsStateSnapshot surface_raster()
{
    return {
        .depth = {true, true, render::DepthCompare::less},
        .cull = render::CullMode::back,
        .front_face = render::FrontFace::counter_clockwise,
        .blend = render::BlendMode::disabled,
        .polygon = opengl::PolygonMode::fill,
    };
}

resources::Diagnostic invalid(std::string message)
{ return {.code = resources::ErrorCode::invalid_argument, .message = std::move(message)}; }

// Texture-unit state is independent of the frame's raster/program scope.
// Restore only the borrowed 2D target, including when it was previously zero.
class SurfaceBinding final {
public:
    SurfaceBinding()
    {
        glGetIntegeri_v(GL_TEXTURE_BINDING_2D, 0, &texture_);
        glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &sampler_);
        glBindSampler(0, 0);
    }
    SurfaceBinding(const SurfaceBinding&) = delete;
    ~SurfaceBinding()
    {
        GLint active{};
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_));
        glActiveTexture(static_cast<GLenum>(active));
        glBindSampler(0, static_cast<GLuint>(sampler_));
    }
private:
    GLint texture_{}, sampler_{};
};

SunMesh corona_quad()
{
    SunMesh mesh(4);
    constexpr std::array<Vec3, 4> points{{{-1.8F,-1.8F,0}, {1.8F,-1.8F,0},
        {1.8F,1.8F,0}, {-1.8F,1.8F,0}}};
    for (u32 i = 0; i < points.size(); ++i) mesh.vertices()[i].set(gfx::Position{}, points[i]);
    mesh.add_face(0, 1, 2); mesh.add_face(0, 2, 3);
    return mesh;
}

struct DrawArguments {
    shaders::Parameters values;
    Mat3 body_transform;
};

DrawArguments arguments(const SunDraw& draw, const render::RenderView& view)
{
    shaders::Parameters parameters;
    // Bound long-running input before float texture coordinates/transcendentals
    // lose precision (or an extreme CLI time could overflow GPU arithmetic).
    const auto time = std::fmod(draw.time, 3600.0F);
    parameters.set(shaders::Time{}, time);
    // One body transform for the surface, its active regions, and every arc.
    // A ten-minute revolution keeps rotation continuous at the hourly reset.
    const auto angle = std::fmod(time, 600.0F) * (2.0F * std::numbers::pi_v<f32> / 600.0F);
    const auto c = std::cos(angle), s = std::sin(angle);
    const Mat3 spin{{Vec3{c, 0, -s}, Vec3{0, 1, 0}, Vec3{s, 0, c}}};
    Mat3 body_transform{};
    for (std::size_t col = 0; col < 3; ++col)
        for (std::size_t row = 0; row < 3; ++row)
            for (std::size_t k = 0; k < 3; ++k)
                body_transform[col][row] += draw.orientation[k][row] * draw.axis_scale[k] * spin[col][k];
    parameters.set(shaders::Displacement{}, draw.displacement);
    parameters.set(shaders::Eye{}, view.camera()->position);
    parameters.set(shaders::StrandHalo{}, 0.0F);
    parameters.set(shaders::WhiteSpots{}, draw.white_spots ? 1.0F : 0.0F);
    parameters.set(shaders::Center{}, draw.position);
    parameters.set(shaders::Radius{}, draw.radius);
    // Project the stretched sphere's covariance into the camera plane. Its
    // symmetric square root gives a corona ellipse matching the body silhouette.
    const auto right=view.camera()->right,up=view.camera()->up;
    double a{},b{},d{};
    for(unsigned c=0;c<3;++c) {
        double x{},y{};
        for(unsigned r=0;r<3;++r) {x+=right[r]*draw.orientation[c][r]*draw.axis_scale[c];y+=up[r]*draw.orientation[c][r]*draw.axis_scale[c];}
        a+=x*x;b+=x*y;d+=y*y;
    }
    const auto determinant=std::sqrt(std::max(0.,a*d-b*b));
    const auto denominator=std::sqrt(a+d+2*determinant);
    Vec3 horizontal{},vertical{};
    for(unsigned r=0;r<3;++r) {
        horizontal[r]=static_cast<f32>((right[r]*(a+determinant)+up[r]*b)/denominator);
        vertical[r]=static_cast<f32>((right[r]*b+up[r]*(d+determinant))/denominator);
    }
    parameters.set(shaders::BillboardRight{}, horizontal);
    parameters.set(shaders::BillboardUp{}, vertical);
    return {parameters, body_transform};
}

Result<void> validate_draw(const SunDraw& draw)
{
    for(unsigned c=0;c<3;++c)if(!std::isfinite(draw.axis_scale[c]) || draw.axis_scale[c]<=0)
        return std::unexpected(invalid("Sun axis scale must be finite and positive"));
    for (std::size_t col = 0; col < 3; ++col)
        for (std::size_t row = 0; row < 3; ++row) {
            f32 dot{};
            for (std::size_t k = 0; k < 3; ++k)
                dot += draw.orientation[col][k] * draw.orientation[row][k];
            if (!std::isfinite(dot) || std::abs(dot - (col == row ? 1.F : 0.F)) > .001F)
                return std::unexpected(invalid("Sun orientation must be an orthonormal rotation"));
        }
    const auto& r = draw.orientation;
    const auto determinant = r[0][0] * (r[1][1] * r[2][2] - r[2][1] * r[1][2]) -
                             r[1][0] * (r[0][1] * r[2][2] - r[2][1] * r[0][2]) +
                             r[2][0] * (r[0][1] * r[1][2] - r[1][1] * r[0][2]);
    if (determinant < 0)
        return std::unexpected(invalid("Sun orientation cannot reflect the surface"));
    if (!std::isfinite(draw.position.x) || !std::isfinite(draw.position.y)
        || !std::isfinite(draw.position.z) || !std::isfinite(draw.radius)
        || draw.radius <= 0.0F || draw.radius > 10000.0F)
        return std::unexpected(invalid("Sun position must be finite and radius must be in (0,10000]"));
    if (!std::isfinite(draw.time) || draw.time < 0.0F || !std::isfinite(draw.displacement)
        || draw.displacement < 0.0F || draw.displacement > 1.0F)
        return std::unexpected(invalid("Sun time must be finite/nonnegative and displacement must be in [0,1]"));
    return {};
}

void hash_bytes(u64& hash, std::span<const std::byte> bytes)
{
    for (const auto byte : bytes) hash = (hash ^ std::to_integer<u8>(byte)) * 0x100000001b3ULL;
}
} // namespace

struct SunRenderer::Impl final {
    Runtime surface;
    Program corona, prominences;
    SunMesh source;
    opengl::GpuMesh<SunVertex> sphere, halo;
    opengl::GpuMesh<ProminenceVertex> loops;
    opengl::Image2D map;
    std::string fingerprint;

    Result<void> validate(const opengl::Frame& frame, const render::RenderView& view) const
    {
        if (!frame.active() || frame.extent().empty() || frame.extent() != view.extent() || !view.camera())
            return std::unexpected(invalid("Sun rendering needs an active frame and matching camera view"));
        if (frame.color_encoding() != render::ColorEncoding::linear)
            return std::unexpected(invalid("Render the sun into linear HDR, then tone-map the result"));
        if (!map.belongs_to(frame.device()))
            return std::unexpected(invalid("Sun resources belong to another OpenGL context"));
        return into_result(frame.device().require_current("SunRenderer"));
    }
};

SunRenderer::SunRenderer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SunRenderer::SunRenderer(SunRenderer&&) noexcept = default;
SunRenderer& SunRenderer::operator=(SunRenderer&&) noexcept = default;
SunRenderer::~SunRenderer() = default;

Result<SunRenderer> SunRenderer::create(opengl::Device& device)
{
    if (auto allowed = device.require_resource_update("SunRenderer::create"); !allowed)
        return std::unexpected(resources::to_diagnostic(std::move(allowed.error())));
    auto surface_code = shaders::surface();
    if (!surface_code) return std::unexpected(std::move(surface_code.error()));
    auto corona_code = shaders::corona();
    if (!corona_code) return std::unexpected(std::move(corona_code.error()));
    auto prominence_code = shaders::prominences();
    if (!prominence_code) return std::unexpected(std::move(prominence_code.error()));
    auto surface = into_result(Runtime::create(device, std::move(*surface_code)));
    if (!surface) return std::unexpected(std::move(surface.error()));
    auto corona = into_result(render::compile_program(device, *corona_code));
    if (!corona) return std::unexpected(std::move(corona.error()));
    auto prominences = into_result(render::compile_program(device, *prominence_code));
    if (!prominences) return std::unexpected(std::move(prominences.error()));

    auto source = make_surface();
    auto sphere = into_result(opengl::upload_mesh(device, source));
    if (!sphere) return std::unexpected(std::move(sphere.error()));
    auto halo = into_result(opengl::upload_mesh(device, corona_quad()));
    if (!halo) return std::unexpected(std::move(halo.error()));
    auto loops = into_result(opengl::upload_mesh(device, make_prominences()));
    if (!loops) return std::unexpected(std::move(loops.error()));
    auto pixels = make_surface_map();
    u64 fingerprint{0xcbf29ce484222325ULL};
    hash_bytes(fingerprint, pixels.pixels);
    hash_bytes(fingerprint, source.vertices().bytes());
    auto map = into_result(gfx::upload_image(device, pixels, {
        .format = gfx::ImageFormat::rgba8, .generate_mipmaps = true,
        .sampler = {.min_filter = gfx::ImageFilter::linear, .mag_filter = gfx::ImageFilter::linear,
            .mip_filter = gfx::MipmapFilter::linear, .wrap_u = gfx::ImageWrap::repeat,
            .wrap_v = gfx::ImageWrap::clamp_to_edge}}));
    if (!map) return std::unexpected(std::move(map.error()));
    if (auto ready = sphere->prepare_vertex_input(device, surface->production()); !ready)
        return std::unexpected(resources::to_diagnostic(std::move(ready.error())));
    if (auto ready = halo->prepare_vertex_input(device, *corona); !ready)
        return std::unexpected(resources::to_diagnostic(std::move(ready.error())));
    if (auto ready = loops->prepare_vertex_input(device, *prominences); !ready)
        return std::unexpected(resources::to_diagnostic(std::move(ready.error())));
    return SunRenderer{std::make_unique<Impl>(std::move(*surface), std::move(*corona),
        std::move(*prominences), std::move(source), std::move(*sphere), std::move(*halo),
        std::move(*loops), std::move(*map), std::to_string(fingerprint))};
}

Result<void> SunRenderer::render(opengl::Frame& frame, const render::RenderView& view, const SunDraw& draw)
{ return render(frame, view, std::span{&draw, 1}); }

Result<void> SunRenderer::render(opengl::Frame& frame, const render::RenderView& view,
    std::span<const SunDraw> draws)
{
    if (!impl_) return std::unexpected(invalid("Sun renderer is empty"));
    if (auto valid = impl_->validate(frame, view); !valid) return valid;
    for (const auto& draw : draws) if (auto valid = validate_draw(draw); !valid) return valid;
    if (draws.empty()) return {};
    SurfaceBinding scope;
    if (auto bound = impl_->map.bind_to_unit(0); !bound) return into_result(std::move(bound));
    auto commands = frame.render_context();
    auto graphics = commands.graphics_state();
    if (auto set = graphics.set(render::FrontFace::counter_clockwise); !set) return into_result(std::move(set));
    if (auto set = graphics.set(opengl::PolygonMode::fill); !set) return into_result(std::move(set));
    for (const auto& draw : draws) {
        auto params = arguments(draw, view);
        // Halo behind the photosphere; sphere then occludes it naturally.
        if (auto set = graphics.set(render::DepthState{true, false}); !set) return into_result(std::move(set));
        if (auto set = graphics.set(render::CullMode::none); !set) return into_result(std::move(set));
        if (auto set = graphics.set(render::BlendMode::additive); !set) return into_result(std::move(set));
        if (auto run = commands.run(impl_->corona, params.values, params.body_transform); !run) return into_result(std::move(run));
        if (auto set = commands.view(view); !set) return into_result(std::move(set));
        if (auto done = commands.draw(impl_->halo); !done) return into_result(std::move(done));

        if (auto set = graphics.set(surface_raster()); !set) return into_result(std::move(set));
        if (auto run = commands.run(impl_->surface.production(), params.values, params.body_transform); !run) return into_result(std::move(run));
        if (auto set = commands.view(view); !set) return into_result(std::move(set));
        if (auto done = commands.draw(impl_->sphere); !done) return into_result(std::move(done));

        // Thin optically translucent plasma strands add light, but do not
        // overwrite depth or make their hidden back side visible through the sun.
        if (auto set = graphics.set(render::DepthState{true, false}); !set) return into_result(std::move(set));
        if (auto set = graphics.set(render::BlendMode::additive); !set) return into_result(std::move(set));
        for (const float halo : {1.0F, 0.0F}) {
            params.values.set(shaders::StrandHalo{}, halo);
            if (auto run = commands.run(impl_->prominences, params.values, params.body_transform); !run) return into_result(std::move(run));
            if (auto set = commands.view(view); !set) return into_result(std::move(set));
            if (auto done = commands.draw(impl_->loops); !done) return into_result(std::move(done));
        }
    }
    return {};
}

Result<analysis::DiagnosticSweep> SunRenderer::diagnose(opengl::Frame& frame,
    const render::RenderView& view, const SunDraw& draw, const analysis::CaptureRequest& request)
{
    if (!impl_) return std::unexpected(invalid("Sun renderer is empty"));
    if (auto valid = impl_->validate(frame, view); !valid) return std::unexpected(std::move(valid.error()));
    if (auto valid = validate_draw(draw); !valid) return std::unexpected(std::move(valid.error()));
    SurfaceBinding scope;
    if (auto bound = impl_->map.bind_to_unit(0); !bound)
        return std::unexpected(resources::to_diagnostic(std::move(bound.error())));
    const auto params = arguments(draw, view);
    if (auto set = impl_->surface.set_arguments(params.values, params.body_transform); !set)
        return std::unexpected(resources::to_diagnostic(std::move(set.error())));
    render::AnalysisOptions options;
    options.label = "displaced solar surface";
    options.resource_fingerprint = impl_->fingerprint;
    Mat4 model;
    for (u32 column = 0; column < 3; ++column) {
        const auto p = params.body_transform[column];
        model[column] = {p.x*draw.radius, p.y*draw.radius, p.z*draw.radius, 0};
    }
    model[3] = {draw.position.x, draw.position.y, draw.position.z, 1};
    options.provenance.object_to_world = model;
    // Diagnose the photosphere's actual pass, not the additive prominence pass
    // left selected by render(). Supplying its policy explicitly lets the
    // runtime scope all native changes without disturbing the caller's state.
    return into_result(impl_->surface.diagnose(frame.device(), impl_->source, impl_->sphere,
        view, opengl::CaptureState{surface_raster(), frame.color_encoding()},
        request, std::move(options)));
}

const SunMesh& SunRenderer::source_mesh() const
{
    if (!impl_) throw std::logic_error("Sun renderer is empty");
    return impl_->source;
}
static_assert(render::RendererFor<SunRenderer, opengl::Frame>);
} // namespace example::sun
