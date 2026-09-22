#include <vng/bloom_opengl/bloom.hpp>

#include <vng/gfx/gfx.hpp>
#include <vng/opengl/buffer.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/framebuffer.hpp>
#include <vng/opengl/gfx_vertex_input.hpp>
#include <vng/opengl/image.hpp>
#include <vng/opengl/program.hpp>
#include <vng/opengl/render_state_scope.hpp>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>

#include "../opengl/gl_error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vng::opengl {
namespace {

struct Position : gfx::Semantic<Vec2> {};
struct UV : gfx::Semantic<Vec2> {};
struct Threshold : gfx::Semantic<f32> {};
struct Strength : gfx::Semantic<f32> {};
struct Exposure : gfx::Semantic<f32> {};
using Parameters = gfx::Record<Threshold, Strength, Exposure>;
using BloomProgram = TypedProgram<Vec2, Parameters>;
using Vertex = gfx::Record<Position, UV>;
using Layout = gfx::VertexLayout<gfx::Stream<Vertex, gfx::PerVertex>>;
using Inputs = shader::VertexInputs<Position, UV>;
using Varyings = shader::VertexOutputs<shader::ClipPosition, shader::smooth<UV>>;
using FragmentInputs = shader::FragmentInputs<shader::smooth<UV>>;
using Outputs = shader::FragmentOutputs<shader::Color<0>>;

resources::Diagnostic invalid(std::string message)
{
    resources::Diagnostic result;
    result.code = resources::ErrorCode::invalid_argument;
    result.message = std::move(message);
    return result;
}

resources::Result<void> valid_extent(Extent2D extent)
{
    if (extent.empty()
        || extent.width > static_cast<u32>(std::numeric_limits<i32>::max())
        || extent.height > static_cast<u32>(std::numeric_limits<i32>::max())) {
        return std::unexpected(invalid("Bloom requires a non-empty representable extent"));
    }
    return {};
}

// A normalized 3x3 tent. Threshold each source tap before filtering so sparse
// bright geometry survives downsampling. The same fixed program handles later
// pyramid levels with a zero threshold.
template<class Stage>
auto filtered(Stage& stage, dsl::Float2 uv, dsl::Float2 texel, dsl::Float threshold)
{
    const auto sample = [&](f32 x, f32 y) {
        // These fixed kernel taps are intentionally baked while building IR;
        // per-pass dimensions and exposure are supplied separately.
        const auto offset = dsl::vec2(texel.x() * stage.constant(x), texel.y() * stage.constant(y));
        const auto color = stage.template sample_2d<0>(uv + offset).xyz();
        const auto brightness = dsl::max(dsl::max(color.x(), color.y()), color.z());
        const auto contribution = dsl::max(brightness - threshold, 0.0F)
            / dsl::max(brightness, 0.00001F);
        return color * contribution;
    };
    return (sample(0, 0) * 4.0F
        + (sample(-1, 0) + sample(1, 0) + sample(0, -1) + sample(0, 1)) * 2.0F
        + sample(-1, -1) + sample(1, -1) + sample(-1, 1) + sample(1, 1)) / 16.0F;
}

template<class Function>
resources::Result<BloomProgram> make_program(
    const Device& device, std::string name, Function&& function)
{
    auto vertex = shader::vertex<Inputs, Varyings>(name + "_vertex", [](auto& stage) {
        return stage.output(
            dsl::field<shader::ClipPosition>(dsl::vec4(stage.input(Position{}), 0.0F, 1.0F)),
            dsl::field<UV>(stage.input(UV{})));
    });
    if (!vertex) return std::unexpected(resources::to_diagnostic(std::move(vertex.error())));
    auto fragment = shader::fragment<FragmentInputs, Outputs>(
        name + "_fragment", std::forward<Function>(function));
    if (!fragment) return std::unexpected(resources::to_diagnostic(std::move(fragment.error())));
    auto linked = shader::link(std::move(*vertex), std::move(*fragment));
    if (!linked) return std::unexpected(resources::to_diagnostic(std::move(linked.error())));
    return resources::into_result(render::compile_program(device, *linked));
}

struct Pipeline {
    BloomProgram downsample;
    BloomProgram upsample;
    BloomProgram composite;
    Buffer vertices;
    VertexArray vao;
};

struct PipelineProvider {
    resources::Result<Pipeline> provide(const Device& device) const
    {
        auto down = make_program(device, "bloom_downsample", [](auto& stage,
            dsl::Float2 texel, dsl::Expr<Parameters> parameters) {
            const auto rgb = filtered(stage, stage.input(UV{}), texel,
                parameters.get(Threshold{}));
            return stage.output(dsl::field<shader::Color<0>>(dsl::vec4(rgb, 1.0F)));
        });
        if (!down) return std::unexpected(std::move(down.error()));
        auto up = make_program(device, "bloom_upsample", [](auto& stage,
            dsl::Float2 texel, dsl::Expr<Parameters>) {
            const auto rgb = filtered(stage, stage.input(UV{}), texel,
                stage.constant(0.0F));
            const auto detail = stage.template sample_2d<1>(stage.input(UV{})).xyz();
            return stage.output(dsl::field<shader::Color<0>>(
                dsl::vec4((rgb + detail) * 0.5F, 1.0F)));
        });
        if (!up) return std::unexpected(std::move(up.error()));
        auto composite = make_program(device, "bloom_composite", [](auto& stage,
            dsl::Float2, dsl::Expr<Parameters> parameters) {
            const auto original = stage.template sample_2d<0>(stage.input(UV{}));
            const auto halo = stage.template sample_2d<1>(stage.input(UV{})).xyz();
            const auto hdr = dsl::max(original.xyz() + halo * parameters.get(Strength{}),
                stage.constant(Vec3{}));
            const auto exposed = dsl::min(hdr * parameters.get(Exposure{}),
                stage.constant(Vec3{1.0e20F, 1.0e20F, 1.0e20F}));
            const auto mapped = exposed / (exposed + stage.constant(Vec3{1.0F, 1.0F, 1.0F}));
            return stage.output(dsl::field<shader::Color<0>>(dsl::vec4(mapped, original.w())));
        });
        if (!composite) return std::unexpected(std::move(composite.error()));
        std::array<Vertex, 3> vertices{};
        constexpr std::array<Vec2, 3> positions{{{-1, -1}, {3, -1}, {-1, 3}}};
        constexpr std::array<Vec2, 3> uvs{{{0, 0}, {2, 0}, {0, 2}}};
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            vertices[i].set(Position{}, positions[i]);
            vertices[i].set(UV{}, uvs[i]);
        }
        auto buffer = resources::into_result(Buffer::create(device,
            {.size = sizeof(vertices), .initial_data = std::as_bytes(std::span{vertices})}));
        if (!buffer) return std::unexpected(std::move(buffer.error()));
        auto vao = resources::into_result(VertexArray::create(device));
        if (!vao) return std::unexpected(std::move(vao.error()));
        const auto layout = gfx::resolve_vertex_input<Inputs, Layout>();
        const std::array bindings{ResolvedStreamBuffer{0, &*buffer, 0}};
        if (auto configured = resources::into_result(configure_vertex_input(*vao, layout, bindings));
            !configured) return std::unexpected(std::move(configured.error()));
        return Pipeline{std::move(*down), std::move(*up), std::move(*composite),
            std::move(*buffer), std::move(*vao)};
    }
};

struct Target {
    Image2D image;
    Framebuffer framebuffer;
};
struct Level { Target down; Target up; };
struct Targets { Extent2D extent; std::vector<Level> levels; };

resources::Result<Target> make_target(const Device& device, Extent2D extent)
{
    auto image = resources::into_result(Image2D::create(
        device, extent.width, extent.height, ImageFormat::rgba16f));
    if (!image) return std::unexpected(std::move(image.error()));
    if (auto filtering = resources::into_result(image->use_linear_filtering()); !filtering)
        return std::unexpected(std::move(filtering.error()));
    auto framebuffer = resources::into_result(Framebuffer::create(device));
    if (!framebuffer) return std::unexpected(std::move(framebuffer.error()));
    if (auto attached = resources::into_result(framebuffer->attach_color(0, *image)); !attached)
        return std::unexpected(std::move(attached.error()));
    if (auto complete = resources::into_result(framebuffer->check_complete()); !complete)
        return std::unexpected(std::move(complete.error()));
    return Target{std::move(*image), std::move(*framebuffer)};
}

struct TargetProvider {
    render::BloomOptions options;

    resources::Result<Targets> provide(const Device& device, Extent2D extent) const
    {
        if (auto valid = valid_extent(extent); !valid) return std::unexpected(std::move(valid.error()));
        if (options.levels < 1 || options.levels > 8)
            return std::unexpected(invalid("Bloom pyramid levels must be in [1, 8]"));
        Targets result{extent, {}};
        result.levels.reserve(options.levels);
        for (u32 level = 0; level < options.levels; ++level) {
            extent = {std::max(1U, extent.width / 2), std::max(1U, extent.height / 2)};
            auto down = make_target(device, extent);
            if (!down) return std::unexpected(std::move(down.error()));
            auto up = make_target(device, extent);
            if (!up) return std::unexpected(std::move(up.error()));
            result.levels.push_back({std::move(*down), std::move(*up)});
            if (extent.width == 1 && extent.height == 1) break;
        }
        return result;
    }
};

// The state scope owns framebuffer/raster state. Texture units and sampler
// objects have an independent boundary, preserving unrelated texture targets.
struct TextureUnits {
    std::array<GLint, 2> textures{}, samplers{};
    TextureUnits()
    {
        for (GLuint unit = 0; unit < 2; ++unit) {
            glGetIntegeri_v(GL_TEXTURE_BINDING_2D, unit, &textures[unit]);
            glGetIntegeri_v(GL_SAMPLER_BINDING, unit, &samplers[unit]);
            glBindSampler(unit, 0);
        }
    }
    ~TextureUnits()
    {
        GLint active{};
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        for (GLuint unit = 0; unit < 2; ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textures[unit]));
            glBindSampler(unit, static_cast<GLuint>(samplers[unit]));
        }
        glActiveTexture(static_cast<GLenum>(active));
    }
};

resources::Result<void> prepare_raster(const Device& device, std::span<const u32> outputs)
{
    if (auto r = resources::into_result(device.set_standard_raster_state()); !r) return r;
    if (auto r = resources::into_result(device.set_rasterizer_discard_enabled(false)); !r) return r;
    if (auto r = resources::into_result(device.set_depth_state({false, false, DepthCompare::always})); !r) return r;
    if (auto r = resources::into_result(device.set_blend_enabled(0, false)); !r) return r;
    if (auto r = resources::into_result(device.set_color_write_mask(0, {true, true, true, true})); !r) return r;
    for (auto output : outputs) {
        if (output == 0) continue;
        if (auto r = resources::into_result(device.set_color_write_mask(output, {false, false, false, false})); !r) return r;
    }
    if (auto r = resources::into_result(device.set_scissor_enabled(false)); !r) return r;
    return resources::into_result(device.set_cull_state({CullMode::none}));
}

} // namespace

struct Bloom::Impl {
    PipelineProvider pipeline_provider;
    TargetProvider target_provider;
    Pipeline pipeline;
    Targets targets;

    resources::Result<void> draw(const Device& device, const BloomProgram& program,
        const Image2D& source, const Image2D& detail, Extent2D destination,
        render::BloomSettings settings)
    {
        const Vec2 texel{1.0F / static_cast<f32>(source.width()),
            1.0F / static_cast<f32>(source.height())};
        Parameters parameters;
        parameters.set(Threshold{}, settings.threshold);
        parameters.set(Strength{}, settings.strength);
        parameters.set(Exposure{}, settings.exposure);
        if (auto r = resources::into_result(program.set_arguments(texel, parameters)); !r) return r;
        if (auto r = resources::into_result(device.viewport(0, 0,
            static_cast<i32>(destination.width), static_cast<i32>(destination.height))); !r) return r;
        if (auto r = resources::into_result(program.untyped().bind()); !r) return r;
        if (auto r = resources::into_result(pipeline.vao.bind()); !r) return r;
        if (auto r = resources::into_result(source.bind_to_unit(0)); !r) return r;
        if (auto r = resources::into_result(detail.bind_to_unit(1)); !r) return r;
        return resources::into_result(device.draw_arrays_instanced(Primitive::triangles, 0, 3));
    }
};

Bloom::Bloom(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Bloom::Bloom(Bloom&&) noexcept = default;
Bloom& Bloom::operator=(Bloom&&) noexcept = default;
Bloom::~Bloom() = default;

resources::Result<Bloom> Bloom::create(
    const Device& device, Extent2D extent, render::BloomOptions options)
{
    if (auto r = resources::into_result(device.require_resource_update("Bloom::create")); !r)
        return std::unexpected(std::move(r.error()));
    TargetProvider target_provider{options};
    auto targets = target_provider.provide(device, extent);
    if (!targets) return std::unexpected(std::move(targets.error()));
    PipelineProvider pipeline_provider;
    auto pipeline = pipeline_provider.provide(device);
    if (!pipeline) return std::unexpected(std::move(pipeline.error()));
    return Bloom{std::make_unique<Impl>(Impl{pipeline_provider, target_provider,
        std::move(*pipeline), std::move(*targets)})};
}

Extent2D Bloom::extent() const noexcept { return impl_ ? impl_->targets.extent : Extent2D{}; }
u32 Bloom::levels() const noexcept { return impl_ ? static_cast<u32>(impl_->targets.levels.size()) : 0; }

resources::Result<void> Bloom::resize(const Device& device, Extent2D extent)
{
    if (!impl_ || !impl_->pipeline.composite.belongs_to(device))
        return std::unexpected(invalid("Bloom::resize requires its owning device and a live effect"));
    if (auto r = resources::into_result(device.require_resource_update("Bloom::resize")); !r) return r;
    if (auto r = valid_extent(extent); !r) return r;
    if (extent == impl_->targets.extent) return {};
    auto candidate = impl_->target_provider.provide(device, extent);
    if (!candidate) return std::unexpected(std::move(candidate.error()));
    impl_->targets = std::move(*candidate);
    return {};
}

resources::Result<void> Bloom::reload(const Device& device)
{
    if (!impl_ || !impl_->pipeline.composite.belongs_to(device))
        return std::unexpected(invalid("Bloom::reload requires its owning device and a live effect"));
    if (auto r = resources::into_result(device.require_resource_update("Bloom::reload")); !r) return r;
    auto targets = impl_->target_provider.provide(device, impl_->targets.extent);
    if (!targets) return std::unexpected(std::move(targets.error()));
    auto pipeline = impl_->pipeline_provider.provide(device);
    if (!pipeline) return std::unexpected(std::move(pipeline.error()));
    impl_->targets = std::move(*targets);
    impl_->pipeline = std::move(*pipeline);
    return {};
}

resources::Result<void> Bloom::apply(Frame& output, const Image2D& source,
    render::BloomSettings settings)
{
    if (!impl_ || !output.active())
        return std::unexpected(invalid("Bloom::apply requires a live effect and active output frame"));
    const auto& device = output.device();
    if (!impl_->pipeline.composite.belongs_to(device) || !source.belongs_to(device))
        return std::unexpected(invalid("Bloom input, effect, and frame must belong to the same device"));
    if (auto r = resources::into_result(device.require_current("Bloom::apply")); !r) return r;
    if (output.extent() != extent() || Extent2D{source.width(), source.height()} != extent())
        return std::unexpected(invalid("Bloom input and output must match the effect extent; resize at a frame boundary"));
    if (source.format() != ImageFormat::rgba16f && source.format() != ImageFormat::rgba32f)
        return std::unexpected(invalid("Bloom input must be a linear RGBA16F or RGBA32F image"));
    if (output.uses_image(source))
        return std::unexpected(invalid("Bloom input cannot also be attached to its output frame"));
    if (!std::isfinite(settings.threshold) || settings.threshold < 0
        || !std::isfinite(settings.strength) || settings.strength < 0
        || !std::isfinite(settings.exposure) || settings.exposure < 0)
        return std::unexpected(invalid("Bloom threshold, strength, and exposure must be finite and nonnegative"));

    GLint output_framebuffer{}, maximum_outputs{}, draw_buffer{}, component_type{};
    if (auto r = resources::into_result(detail::checked_gl_call("Bloom output format", [&] {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &output_framebuffer);
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maximum_outputs);
        glGetIntegerv(GL_DRAW_BUFFER0, &draw_buffer);
        if (draw_buffer != GL_NONE) {
            auto attachment = static_cast<GLenum>(draw_buffer);
            if (output_framebuffer == 0) {
                if (attachment == GL_BACK) attachment = GL_BACK_LEFT;
                if (attachment == GL_FRONT || attachment == GL_FRONT_AND_BACK) attachment = GL_FRONT_LEFT;
            }
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component_type);
        }
    })); !r) return r;
    if (maximum_outputs <= 0 || draw_buffer == GL_NONE
        || (component_type != GL_FLOAT && component_type != GL_UNSIGNED_NORMALIZED
            && component_type != GL_SIGNED_NORMALIZED)) {
        return std::unexpected(invalid("Bloom output 0 requires a normalized or floating color attachment"));
    }
    std::vector<u32> attachments(static_cast<std::size_t>(maximum_outputs));
    for (u32 i = 0; i < attachments.size(); ++i) attachments[i] = i;
    auto state = resources::into_result(RenderStateScope::capture(device, attachments));
    if (!state) return std::unexpected(std::move(state.error()));
    TextureUnits units;
    // Internal targets need no nested Frame or replacement command context.
    // The explicit scope restores native state and invalidates managed binding
    // caches; the caller's frame handles remain usable after this pass.
    if (auto r = prepare_raster(device, attachments); !r) return r;
    if (auto r = resources::into_result(device.set_framebuffer_srgb_enabled(false)); !r) return r;

    const Image2D* halo = &source;
    if (settings.strength > 0) {
        bool first = true;
        for (auto& level : impl_->targets.levels) {
            if (auto r = resources::into_result(level.down.framebuffer.bind()); !r) return r;
            auto pass = settings;
            pass.threshold = first ? settings.threshold : 0.0F;
            if (auto r = impl_->draw(device, impl_->pipeline.downsample, *halo, *halo,
                {level.down.image.width(), level.down.image.height()}, pass); !r) return r;
            halo = &level.down.image;
            first = false;
        }
        for (std::size_t i = impl_->targets.levels.size() - 1; i > 0; --i) {
            auto& level = impl_->targets.levels[i - 1];
            if (auto r = resources::into_result(level.up.framebuffer.bind()); !r) return r;
            if (auto r = impl_->draw(device, impl_->pipeline.upsample, *halo, level.down.image,
                {level.up.image.width(), level.up.image.height()}, settings); !r) return r;
            halo = &level.up.image;
        }
    }
    if (auto r = resources::into_result(detail::checked_gl_call("Bloom output target", [&] {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(output_framebuffer));
    })); !r) return r;
    if (auto r = resources::into_result(device.set_framebuffer_srgb_enabled(
        output.color_encoding() == render::ColorEncoding::srgb)); !r) return r;
    if (auto r = impl_->draw(device, impl_->pipeline.composite, source, *halo,
        extent(), settings); !r) return r;
    return resources::into_result(state->restore());
}

resources::Result<Bloom> BloomBuilder::build(Extent2D extent) const
{
    return Bloom::create(*device_, extent, options_);
}

} // namespace vng::opengl
