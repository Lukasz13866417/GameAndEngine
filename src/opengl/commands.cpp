#include <vng/opengl/commands.hpp>

#include "context_state.hpp"

#include <string>
#include <utility>

namespace vng::opengl {
namespace {

[[nodiscard]] std::expected<DepthCompare, Diagnostic> translate_compare(
    render::DepthCompare compare)
{
    switch (compare) {
    case render::DepthCompare::never:
        return DepthCompare::never;
    case render::DepthCompare::less:
        return DepthCompare::less;
    case render::DepthCompare::less_equal:
        return DepthCompare::less_equal;
    case render::DepthCompare::equal:
        return DepthCompare::equal;
    case render::DepthCompare::greater_equal:
        return DepthCompare::greater_equal;
    case render::DepthCompare::greater:
        return DepthCompare::greater;
    case render::DepthCompare::not_equal:
        return DepthCompare::not_equal;
    case render::DepthCompare::always:
        return DepthCompare::always;
    }
    return std::unexpected(Diagnostic{
        .code = ErrorCode::invalid_argument,
        .message = "OpenGL Commands received an invalid depth comparison",
    });
}

[[nodiscard]] std::expected<CullMode, Diagnostic> translate_cull(
    render::CullMode mode)
{
    switch (mode) {
    case render::CullMode::none:
        return CullMode::none;
    case render::CullMode::front:
        return CullMode::front;
    case render::CullMode::back:
        return CullMode::back;
    }
    return std::unexpected(Diagnostic{
        .code = ErrorCode::invalid_argument,
        .message = "OpenGL Commands received an invalid cull mode",
    });
}

[[nodiscard]] std::expected<FrontFaceWinding, Diagnostic>
translate_front_face(render::FrontFace winding)
{
    switch (winding) {
    case render::FrontFace::clockwise:
        return FrontFaceWinding::clockwise;
    case render::FrontFace::counter_clockwise:
        return FrontFaceWinding::counter_clockwise;
    }
    return std::unexpected(Diagnostic{
        .code = ErrorCode::invalid_argument,
        .message = "OpenGL Commands received an invalid front-face winding",
    });
}

} // namespace

Commands::Commands(
    const Device& device,
    u64 frame_generation,
    u64 command_epoch,
    Extent2D extent,
    render::ColorEncoding color_encoding) noexcept
    : device_(device.state_),
      frame_generation_(frame_generation),
      command_epoch_(command_epoch),
      extent_(extent),
      color_encoding_(color_encoding)
{}

Commands::Commands(Commands&& other) noexcept
    : device_(std::move(other.device_)),
      frame_generation_(std::exchange(other.frame_generation_, 0)),
      command_epoch_(std::exchange(other.command_epoch_, 0)),
      extent_(std::exchange(other.extent_, {})),
      color_encoding_(std::exchange(
          other.color_encoding_, render::ColorEncoding::srgb)),
      pipeline_(std::exchange(other.pipeline_, nullptr)),
      view_ready_(std::exchange(other.view_ready_, false))
{}

Commands& Commands::operator=(Commands&& other) noexcept
{
    if (this != &other) {
        device_ = std::move(other.device_);
        frame_generation_ = std::exchange(other.frame_generation_, 0);
        command_epoch_ = std::exchange(other.command_epoch_, 0);
        extent_ = std::exchange(other.extent_, {});
        color_encoding_ = std::exchange(
            other.color_encoding_, render::ColorEncoding::srgb);
        pipeline_ = std::exchange(other.pipeline_, nullptr);
        view_ready_ = std::exchange(other.view_ready_, false);
    }
    return *this;
}

bool Commands::active() const noexcept
{
    return device_.state_
        && device_.state_->access.valid()
        && device_.state_->command_stream_is_active(
            frame_generation_, command_epoch_);
}

std::expected<void, Diagnostic> Commands::validate(
    const char* operation) const
{
    if (auto current = device_.require_current(operation); !current) {
        return current;
    }
    if (!device_.state_
        || !device_.state_->command_stream_is_active(
            frame_generation_, command_epoch_)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string{operation}
                + " used a stale or ended OpenGL frame command stream",
        });
    }
    return {};
}

std::expected<void, Diagnostic> Commands::validate_encoding(
    render::ColorEncoding encoding,
    const char* operation) const
{
    switch (encoding) {
    case render::ColorEncoding::linear:
    case render::ColorEncoding::srgb:
        break;
    default:
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string{operation}
                + " received an invalid output encoding",
        });
    }
    if (encoding != color_encoding_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string{operation}
                + " requires the pipeline output encoding to match the frame target encoding",
        });
    }
    return {};
}

std::expected<void, Diagnostic> Commands::require_pipeline(
    const char* operation) const
{
    if (auto valid = validate(operation); !valid) {
        return valid;
    }
    if (pipeline_ == nullptr) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string{operation}
                + " requires bind(pipeline) first",
        });
    }
    if (!pipeline_->belongs_to(device_)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::incompatible_device,
            .message = std::string{operation}
                + " found a bound pipeline from a different OpenGL device/context",
        });
    }
    return validate_encoding(
        pipeline_->description().output_encoding, operation);
}

void Commands::invalidate_binding() noexcept
{
    pipeline_ = nullptr;
    view_ready_ = false;
}

std::expected<void, Diagnostic> Commands::bind(
    const GraphicsPipeline& pipeline)
{
    if (auto valid = validate("OpenGL Commands::bind"); !valid) {
        return valid;
    }
    if (!pipeline.belongs_to(device_)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::incompatible_device,
            .message = "OpenGL Commands::bind received a pipeline from a different device/context",
        });
    }
    if (auto encoding = validate_encoding(
            pipeline.description().output_encoding,
            "OpenGL Commands::bind");
        !encoding) {
        return encoding;
    }

    // From here on OpenGL may have been changed even when an operation
    // reports failure. Drop the old logical binding first so a failed switch
    // can never leave this stream claiming that its previous pipeline is
    // still active.
    invalidate_binding();
    if (auto bound = pipeline.bind(device_); !bound) {
        return bound;
    }

    pipeline_ = &pipeline;
    view_ready_ = true;
    if (const auto* source = pipeline.generated_source()) {
        for (const auto& parameter : source->parameters) {
            if (parameter.kind
                == shader::ParameterKind::camera_view_projection) {
                view_ready_ = false;
                break;
            }
        }
    }
    return {};
}

std::expected<void, Diagnostic> Commands::view(
    const render::RenderView& view)
{
    if (auto ready = require_pipeline("OpenGL Commands::view"); !ready) {
        return ready;
    }

    // A new view request supersedes the old snapshot even when the request is
    // invalid. This prevents callers that accidentally ignore an error from
    // drawing with stale camera parameters from an earlier view.
    view_ready_ = false;
    if (view.extent() != extent_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "OpenGL Commands::view requires matching frame and view extents",
        });
    }

    const auto* source = pipeline_->generated_source();
    if (source == nullptr) {
        // Expert raw-GLSL pipelines have no portable parameter contract. They
        // remain drawable, but pretending to upload a supplied camera would
        // hide the fact that parameter binding is necessarily expert-owned.
        if (view.camera()) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "OpenGL Commands::view cannot bind a camera to an expert raw-GLSL pipeline because it has no generated parameter metadata",
            });
        }
        view_ready_ = true;
        return {};
    }

    for (const auto& parameter : source->parameters) {
        switch (parameter.kind) {
        case shader::ParameterKind::camera_view_projection:
            if (!view.camera()) {
                return std::unexpected(Diagnostic{
                    .code = ErrorCode::invalid_argument,
                    .message = "OpenGL Commands::view requires a camera because the bound shader reads camera parameters",
                });
            }
            if (auto uploaded = pipeline_->program().set_uniform_mat4(
                    parameter.location,
                    view.camera()->view_projection);
                !uploaded) {
                return uploaded;
            }
            break;
        }
    }
    view_ready_ = true;
    return {};
}

std::expected<void, Diagnostic> Commands::state(
    render::GraphicsPipelineDesc requested)
{
    if (auto ready = require_pipeline("OpenGL Commands::state"); !ready) {
        return ready;
    }
    if (auto encoding = validate_encoding(
            requested.output_encoding, "OpenGL Commands::state");
        !encoding) {
        return encoding;
    }
    auto compare = translate_compare(requested.depth.compare);
    if (!compare) {
        return std::unexpected(std::move(compare.error()));
    }
    auto mode = translate_cull(requested.cull);
    if (!mode) {
        return std::unexpected(std::move(mode.error()));
    }
    auto winding = translate_front_face(requested.front_face);
    if (!winding) {
        return std::unexpected(std::move(winding.error()));
    }

    // Re-establish the shader's complete baseline first. That makes state()
    // deterministic even if expert OpenGL code touched ambient state between
    // command calls; the requested portable differences are applied below.
    if (auto baseline = pipeline_->bind(device_); !baseline) {
        invalidate_binding();
        return baseline;
    }
    if (auto changed = device_.set_depth_state(DepthState{
            .test_enabled = requested.depth.test,
            .write_enabled = requested.depth.write,
            .compare = *compare,
        });
        !changed) {
        invalidate_binding();
        return changed;
    }
    auto changed = device_.set_cull_state(CullState{
        .mode = *mode,
        .front_face = *winding,
    });
    if (!changed) {
        invalidate_binding();
    }
    return changed;
}

std::expected<void, Diagnostic> Commands::depth(render::DepthState state)
{
    if (auto ready = require_pipeline("OpenGL Commands::depth"); !ready) {
        return ready;
    }
    auto compare = translate_compare(state.compare);
    if (!compare) {
        return std::unexpected(std::move(compare.error()));
    }
    auto changed = device_.set_depth_state(DepthState{
        .test_enabled = state.test,
        .write_enabled = state.write,
        .compare = *compare,
    });
    if (!changed) {
        invalidate_binding();
    }
    return changed;
}

std::expected<void, Diagnostic> Commands::cull(
    render::CullMode mode,
    render::FrontFace front_face)
{
    if (auto ready = require_pipeline("OpenGL Commands::cull"); !ready) {
        return ready;
    }
    auto translated_mode = translate_cull(mode);
    if (!translated_mode) {
        return std::unexpected(std::move(translated_mode.error()));
    }
    auto translated_winding = translate_front_face(front_face);
    if (!translated_winding) {
        return std::unexpected(std::move(translated_winding.error()));
    }
    auto changed = device_.set_cull_state(CullState{
        .mode = *translated_mode,
        .front_face = *translated_winding,
    });
    if (!changed) {
        invalidate_binding();
    }
    return changed;
}

std::expected<const GraphicsPipeline*, Diagnostic>
Commands::prepare_draw() const
{
    if (auto ready = require_pipeline("OpenGL Commands::draw"); !ready) {
        return std::unexpected(std::move(ready.error()));
    }
    if (!view_ready_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "OpenGL Commands::draw requires view(render_view) because the bound shader reads camera parameters",
        });
    }
    return pipeline_;
}

} // namespace vng::opengl
