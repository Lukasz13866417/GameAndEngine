#include <vng/opengl/commands.hpp>

#include "context_state.hpp"

#include <string>
#include <utility>

namespace vng::opengl {
namespace {

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
    Extent2D extent,
    render::ColorEncoding color_encoding) noexcept
    : device_(device.state_),
      frame_generation_(frame_generation),
      extent_(extent),
      color_encoding_(color_encoding)
{}

Commands::Commands(Commands&& other) noexcept
    : device_(std::move(other.device_)),
      frame_generation_(std::exchange(other.frame_generation_, 0)),
      extent_(std::exchange(other.extent_, {})),
      color_encoding_(std::exchange(
          other.color_encoding_, render::ColorEncoding::srgb))
{}

Commands& Commands::operator=(Commands&& other) noexcept
{
    if (this != &other) {
        device_ = std::move(other.device_);
        frame_generation_ = std::exchange(other.frame_generation_, 0);
        extent_ = std::exchange(other.extent_, {});
        color_encoding_ = std::exchange(
            other.color_encoding_, render::ColorEncoding::srgb);
    }
    return *this;
}

bool Commands::active() const noexcept
{
    return device_.state_
        && device_.state_->access.valid()
        && device_.state_->frame_is_active(frame_generation_);
}

std::expected<void, Diagnostic> Commands::validate(
    const char* operation) const
{
    if (auto current = device_.require_current(operation); !current) {
        return current;
    }
    if (!device_.state_
        || !device_.state_->frame_is_active(frame_generation_)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string{operation}
                + " used a stale or ended OpenGL frame command stream",
        });
    }
    return {};
}

std::expected<void, Diagnostic> Commands::require_program(
    const char* operation) const
{
    if (auto valid = validate(operation); !valid) {
        return valid;
    }
    if (device_.state_->command_program == nullptr) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string{operation}
                + " requires run(program) or bind(program) first",
        });
    }
    if (!device_.state_->command_program->belongs_to(device_) || device_.state_->command_program->native_handle() == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::incompatible_device,
            .message = std::string{operation}
                + " found an empty program or one from a different OpenGL device/context",
        });
    }
    return {};
}

void Commands::invalidate_binding() noexcept
{
    device_.state_->command_program = nullptr;
    device_.state_->command_view_ready = false;
}

GraphicsState Commands::graphics_state() const & noexcept
{
    return GraphicsState{detail::GraphicsStateAccess{
        device_.state_, frame_generation_}};
}

std::expected<void, Diagnostic> Commands::run(const Program& program)
{
    if (auto valid = validate("OpenGL Commands::run"); !valid) return valid;
    if (device_.state_->command_program == &program) {
        if (auto ready = require_program("OpenGL Commands::run"); !ready) return ready;
        return detail::GraphicsStateAccess{
            device_.state_, frame_generation_}.synchronize();
    }
    return bind(program);
}

std::expected<void, Diagnostic> Commands::bind(const Program& program)
{
    if (auto valid = validate("OpenGL Commands::bind"); !valid) return valid;
    if (!program.belongs_to(device_) || program.native_handle() == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::incompatible_device,
            .message = "OpenGL Commands::bind received an empty program or one from a different device/context",
        });
    }
    invalidate_binding();
    if (auto ready = detail::GraphicsStateAccess{
            device_.state_, frame_generation_}.synchronize(); !ready) {
        return ready;
    }
    if (auto bound = program.bind(); !bound) return bound;
    device_.state_->command_program = &program;
    device_.state_->command_view_ready = true;
    if (const auto* source = program.generated_source()) {
        for (const auto& parameter : source->parameters) {
            if (parameter.kind == shader::ParameterKind::camera_view_projection) {
                device_.state_->command_view_ready = false;
            }
        }
    }
    return {};
}

std::expected<void, Diagnostic> Commands::restore()
{
    if (auto valid = require_program("OpenGL Commands::restore"); !valid) return valid;
    const auto* program = device_.state_->command_program;
    device_.state_->graphics_synchronized = false;
    if (auto encoding = device_.set_framebuffer_srgb_enabled(
            color_encoding_ == render::ColorEncoding::srgb); !encoding) {
        invalidate_binding();
        return encoding;
    }
    return bind(*program);
}

std::expected<void, Diagnostic> Commands::view(
    const render::RenderView& view)
{
    if (auto ready = require_program("OpenGL Commands::view"); !ready) {
        return ready;
    }

    // A new view request supersedes the old snapshot even when the request is
    // invalid. This prevents callers that accidentally ignore an error from
    // drawing with stale camera parameters from an earlier view.
    device_.state_->command_view_ready = false;
    if (view.extent() != extent_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "OpenGL Commands::view requires matching frame and view extents",
        });
    }

    const auto* source = device_.state_->command_program->generated_source();
    if (source == nullptr) {
        // Expert raw-GLSL programs have no portable parameter contract. They
        // remain drawable, but pretending to upload a supplied camera would
        // hide the fact that parameter binding is necessarily expert-owned.
        if (view.camera()) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "OpenGL Commands::view cannot bind a camera to an expert raw-GLSL program because it has no generated parameter metadata",
            });
        }
        device_.state_->command_view_ready = true;
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
            if (auto uploaded = device_.state_->command_program->set_uniform_mat4(
                    parameter.location,
                    view.camera()->view_projection);
                !uploaded) {
                return uploaded;
            }
            break;
        case shader::ParameterKind::argument:
            break; // Supplied explicitly by run(program, values...).
        }
    }
    device_.state_->command_view_ready = true;
    return {};
}

std::expected<void, Diagnostic> Commands::depth(render::DepthState state)
{
    return graphics_state().set(state);
}

std::expected<void, Diagnostic> Commands::cull(
    render::CullMode mode,
    render::FrontFace front_face)
{
    if (auto ready = validate("OpenGL Commands::cull"); !ready) {
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
    auto graphics = graphics_state();
    if (auto changed = graphics.set(mode); !changed) return changed;
    return graphics.set(front_face);
}

std::expected<const Program*, Diagnostic>
Commands::prepare_draw() const
{
    if (auto ready = require_program("OpenGL Commands::draw"); !ready) {
        return std::unexpected(std::move(ready.error()));
    }
    if (!device_.state_->command_view_ready) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "OpenGL Commands::draw requires view(render_view) because the bound shader reads camera parameters",
        });
    }
    if (!device_.state_->command_program->arguments_ready()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "OpenGL Commands::draw requires the shader's typed arguments",
        });
    }
    if (auto ready = detail::GraphicsStateAccess{
            device_.state_, frame_generation_}.synchronize(); !ready) {
        return std::unexpected(std::move(ready.error()));
    }
    return device_.state_->command_program;
}

} // namespace vng::opengl
