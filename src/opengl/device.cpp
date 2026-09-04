#include <vng/opengl/device.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace vng::opengl {
namespace {

// A context has one set of OpenGL state and, in particular, only one debug
// callback.  CurrentContextAccess is intentionally copyable, so callers can
// legitimately create more than one Device facade for the same context.  Keep
// those facades on one shared ContextState instead of letting each one replace
// the callback and later tear down a callback owned by another Device.
std::mutex context_states_mutex;
std::unordered_map<const ContextLifetime*, std::weak_ptr<detail::ContextState>>
    context_states;

[[nodiscard]] DebugSeverity debug_severity(GLenum severity) noexcept {
    switch (severity) {
    case GL_DEBUG_SEVERITY_NOTIFICATION: return DebugSeverity::notification;
    case GL_DEBUG_SEVERITY_LOW: return DebugSeverity::low;
    case GL_DEBUG_SEVERITY_MEDIUM: return DebugSeverity::medium;
    case GL_DEBUG_SEVERITY_HIGH: return DebugSeverity::high;
    default: return DebugSeverity::unknown;
    }
}

[[nodiscard]] const char* debug_source(GLenum source) noexcept {
    switch (source) {
    case GL_DEBUG_SOURCE_API: return "api";
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM: return "window-system";
    case GL_DEBUG_SOURCE_SHADER_COMPILER: return "shader-compiler";
    case GL_DEBUG_SOURCE_THIRD_PARTY: return "third-party";
    case GL_DEBUG_SOURCE_APPLICATION: return "application";
    case GL_DEBUG_SOURCE_OTHER: return "other";
    default: return "unknown";
    }
}

[[nodiscard]] const char* debug_type(GLenum type) noexcept {
    switch (type) {
    case GL_DEBUG_TYPE_ERROR: return "error";
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "deprecated-behavior";
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: return "undefined-behavior";
    case GL_DEBUG_TYPE_PORTABILITY: return "portability";
    case GL_DEBUG_TYPE_PERFORMANCE: return "performance";
    case GL_DEBUG_TYPE_MARKER: return "marker";
    case GL_DEBUG_TYPE_PUSH_GROUP: return "push-group";
    case GL_DEBUG_TYPE_POP_GROUP: return "pop-group";
    case GL_DEBUG_TYPE_OTHER: return "other";
    default: return "unknown";
    }
}

void debug_callback(
    GLenum source,
    GLenum type,
    GLuint id,
    GLenum severity,
    GLsizei length,
    const GLchar* message,
    const void* user) noexcept {
    auto* diagnostics =
        static_cast<detail::DiagnosticStore*>(const_cast<void*>(user));
    if (diagnostics == nullptr) {
        return;
    }
    try {
        std::lock_guard lock(diagnostics->mutex);
        diagnostics->debug_messages.push_back(DebugMessage{
            .id = id,
            .severity = debug_severity(severity),
            .source = debug_source(source),
            .type = debug_type(type),
            .message = message == nullptr
                ? std::string{}
                : std::string(message, length >= 0 ? static_cast<std::size_t>(length) : 0),
        });
    } catch (...) {
        // Never propagate through the C driver callback.
    }
}

[[nodiscard]] GLenum gl_primitive(Primitive primitive) noexcept {
    switch (primitive) {
    case Primitive::points: return GL_POINTS;
    case Primitive::lines: return GL_LINES;
    case Primitive::line_strip: return GL_LINE_STRIP;
    case Primitive::triangles: return GL_TRIANGLES;
    case Primitive::triangle_strip: return GL_TRIANGLE_STRIP;
    }
    return GL_TRIANGLES;
}

[[nodiscard]] GLenum gl_depth_compare(DepthCompare compare) noexcept {
    switch (compare) {
    case DepthCompare::never: return GL_NEVER;
    case DepthCompare::less: return GL_LESS;
    case DepthCompare::less_equal: return GL_LEQUAL;
    case DepthCompare::equal: return GL_EQUAL;
    case DepthCompare::greater_equal: return GL_GEQUAL;
    case DepthCompare::greater: return GL_GREATER;
    case DepthCompare::not_equal: return GL_NOTEQUAL;
    case DepthCompare::always: return GL_ALWAYS;
    }
    return GL_LESS;
}

void set_capability(GLenum capability, bool enabled) noexcept {
    if (enabled) {
        glEnable(capability);
    } else {
        glDisable(capability);
    }
}

[[nodiscard]] std::expected<void, Diagnostic> validate_draw_buffer_index(
    std::uint32_t color_attachment,
    std::string_view operation,
    int maximum_draw_buffers) {
    if (maximum_draw_buffers <= 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported an invalid GL_MAX_DRAW_BUFFERS while "
                + std::string(operation),
        });
    }
    if (color_attachment >= static_cast<std::uint32_t>(maximum_draw_buffers)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = std::string(operation)
                + " color attachment exceeds GL_MAX_DRAW_BUFFERS",
        });
    }
    return {};
}

[[nodiscard]] std::expected<DefaultFramebufferCapabilities, Diagnostic>
query_default_framebuffer_capabilities(int major) {
    DefaultFramebufferCapabilities capabilities{
        .color_encoding = std::nullopt,
        .srgb_conversion_supported = major >= 3,
    };

    GLint previous_draw_framebuffer = 0;
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING)",
            [&] {
                glGetIntegerv(
                    GL_DRAW_FRAMEBUFFER_BINDING,
                    &previous_draw_framebuffer);
            });
        !queried) {
        return std::unexpected(std::move(queried.error()));
    }

    if (previous_draw_framebuffer != 0) {
        if (auto bound = detail::checked_gl_call(
                "glBindFramebuffer(default framebuffer capability query)",
                [] { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); });
            !bound) {
            return std::unexpected(std::move(bound.error()));
        }
    }

    const auto restore_binding = [&]() -> std::expected<void, Diagnostic> {
        if (previous_draw_framebuffer == 0) {
            return {};
        }
        return detail::checked_gl_call(
            "glBindFramebuffer(restore after default framebuffer capability query)",
            [&] {
                glBindFramebuffer(
                    GL_DRAW_FRAMEBUFFER,
                    static_cast<GLuint>(previous_draw_framebuffer));
            });
    };

    GLint double_buffered = GL_FALSE;
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_DOUBLEBUFFER)",
            [&] { glGetIntegerv(GL_DOUBLEBUFFER, &double_buffered); });
        !queried) {
        const auto original = std::move(queried.error());
        if (auto restored = restore_binding(); !restored) {
            return std::unexpected(std::move(restored.error()));
        }
        return std::unexpected(original);
    }

    GLint encoding = 0;
    detail::clear_gl_errors();
    glGetFramebufferAttachmentParameteriv(
        GL_DRAW_FRAMEBUFFER,
        double_buffered == GL_TRUE ? GL_BACK_LEFT : GL_FRONT_LEFT,
        GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,
        &encoding);
    const GLenum query_error = glGetError();
    while (glGetError() != GL_NO_ERROR) {
    }

    if (auto restored = restore_binding(); !restored) {
        return std::unexpected(std::move(restored.error()));
    }

    // A surfaceless context may have no default color attachment. That is a
    // valid Device for off-screen work; leave the physical encoding unknown so
    // beginning a default-target Frame rejects it explicitly.
    if (query_error != GL_NO_ERROR) {
        return capabilities;
    }

    switch (encoding) {
    case GL_LINEAR:
        capabilities.color_encoding = render::ColorEncoding::linear;
        break;
    case GL_SRGB:
        capabilities.color_encoding = render::ColorEncoding::srgb;
        break;
    default:
        break;
    }
    return capabilities;
}

} // namespace

namespace detail {

ContextState::~ContextState() {
    if (debug_output_installed && is_current()) {
        glDebugMessageCallback(nullptr, nullptr);
        glDisable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDisable(GL_DEBUG_OUTPUT);
    }
}

} // namespace detail

Device::Device(std::shared_ptr<detail::ContextState> state) noexcept
    : state_(std::move(state)) {}

Device::~Device() = default;

std::expected<Device, Diagnostic> Device::create(
    CurrentContextAccess access,
    DeviceOptions options) {
    if (options.required_major < 4 ||
        (options.required_major == 4 && options.required_minor < 6)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "The vng OpenGL backend requires OpenGL 4.6 core; its required version cannot be lowered",
        });
    }
    if (!access.valid()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Device::create received an incomplete or expired context token",
        });
    }
    if (std::this_thread::get_id() != access.owner_thread) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::wrong_thread,
            .message = "Device::create must run on the context token's owner thread",
        });
    }
    if (!access.is_current(access.lifetime->native_identity())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::context_not_current,
            .message = "Device::create requires the represented context to be current",
        });
    }
    if (access.required_default_framebuffer_encoding) {
        switch (*access.required_default_framebuffer_encoding) {
        case render::ColorEncoding::linear:
        case render::ColorEncoding::srgb:
            break;
        default:
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_context_access,
                .message = "Device::create received an invalid required default-framebuffer encoding",
            });
        }
    }

    const auto loaded_version = gladLoadGL(
        reinterpret_cast<GLADloadfunc>(access.resolve));
    if (loaded_version == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::procedure_loading_failed,
            .message = "GLAD failed to load OpenGL procedures for the current context",
        });
    }

    GLint major = 0;
    GLint minor = 0;
    GLint profile = 0;
    GLint maximum_draw_buffers = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maximum_draw_buffers);
    if (major < options.required_major
        || (major == options.required_major && minor < options.required_minor)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::unsupported_version,
            .message = "OpenGL " + std::to_string(options.required_major) + "."
                + std::to_string(options.required_minor) + " was requested, but the context is "
                + std::to_string(major) + "." + std::to_string(minor),
        });
    }
    if ((profile & GL_CONTEXT_CORE_PROFILE_BIT) == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::unsupported_version,
            .message = "The current OpenGL context is not a core-profile context",
        });
    }
    if (maximum_draw_buffers <= 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported an invalid GL_MAX_DRAW_BUFFERS",
        });
    }

    auto default_framebuffer = query_default_framebuffer_capabilities(major);
    if (!default_framebuffer) {
        return std::unexpected(std::move(default_framebuffer.error()));
    }
    if (access.required_default_framebuffer_encoding
        && !default_framebuffer->supports(
            *access.required_default_framebuffer_encoding)) {
        const auto required = *access.required_default_framebuffer_encoding
                == render::ColorEncoding::srgb
            ? "sRGB"
            : "linear";
        const auto actual = !default_framebuffer->color_encoding
            ? "unavailable"
            : *default_framebuffer->color_encoding
                    == render::ColorEncoding::srgb
                ? "sRGB"
                : "linear";
        const bool missing_srgb_conversion =
            *access.required_default_framebuffer_encoding
                == render::ColorEncoding::srgb
            && !default_framebuffer->srgb_conversion_supported;
        return std::unexpected(Diagnostic{
            .code = ErrorCode::unsupported_feature,
            .message = std::string(
                "Device::create required a ")
                + required
                + " default framebuffer, but OpenGL reports "
                + actual
                + (missing_srgb_conversion
                    ? " without framebuffer sRGB conversion support"
                    : ""),
        });
    }

    std::shared_ptr<detail::ContextState> state;
    {
        std::lock_guard lock(context_states_mutex);
        for (auto entry = context_states.begin(); entry != context_states.end();) {
            if (entry->second.expired()) {
                entry = context_states.erase(entry);
            } else {
                ++entry;
            }
        }
        auto& registered = context_states[access.lifetime.get()];
        state = registered.lock();
        if (!state) {
            state = std::make_shared<detail::ContextState>();
            state->access = access;
            state->major = major;
            state->minor = minor;
            state->maximum_draw_buffers = maximum_draw_buffers;
            state->default_framebuffer = *default_framebuffer;
            state->diagnostics = std::make_shared<detail::DiagnosticStore>();
            registered = state;
        } else if (state->default_framebuffer != *default_framebuffer) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_context_access,
                .message = "Device::create observed conflicting default-framebuffer capabilities for an existing context",
            });
        }

        if (options.enable_debug_output && GLAD_GL_VERSION_4_3) {
            if (!state->debug_output_installed) {
                state->access.lifetime->retain(state->diagnostics);
                glEnable(GL_DEBUG_OUTPUT);
                if (!options.synchronous_debug_output) {
                    glDisable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
                }
                glDebugMessageCallback(debug_callback, state->diagnostics.get());
                state->debug_output_installed = true;
            }
            // Debug output is context state. Once any facade asks for
            // synchronous delivery, retaining that stronger setting is the
            // only choice that cannot invalidate an existing facade's
            // expectations.
            if (options.synchronous_debug_output) {
                glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            }
        }
    }

    return Device(std::move(state));
}

int Device::major_version() const noexcept {
    return state_ ? state_->major : 0;
}

int Device::minor_version() const noexcept {
    return state_ ? state_->minor : 0;
}

DefaultFramebufferCapabilities
Device::default_framebuffer_capabilities() const noexcept {
    return state_ ? state_->default_framebuffer
                  : DefaultFramebufferCapabilities{};
}

bool Device::is_current() const noexcept {
    return state_ && state_->is_current();
}

std::expected<void, Diagnostic> Device::require_current(
    std::string_view operation) const {
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = std::string(operation) + ": device is empty",
        });
    }
    return state_->require_current(operation);
}

std::vector<DebugMessage> Device::take_debug_messages() const {
    if (!state_ || !state_->diagnostics) {
        return {};
    }
    std::lock_guard lock(state_->diagnostics->mutex);
    const auto begin = state_->diagnostics->debug_messages.begin()
        + static_cast<std::ptrdiff_t>(
            state_->diagnostics->consumed_debug_messages);
    std::vector<DebugMessage> result(
        begin,
        state_->diagnostics->debug_messages.end());
    state_->diagnostics->consumed_debug_messages =
        state_->diagnostics->debug_messages.size();
    return result;
}

std::vector<Diagnostic> Device::take_lifecycle_diagnostics() const {
    if (!state_ || !state_->diagnostics) {
        return {};
    }
    std::lock_guard lock(state_->diagnostics->mutex);
    const auto begin = state_->diagnostics->lifecycle_diagnostics.begin()
        + static_cast<std::ptrdiff_t>(
            state_->diagnostics->consumed_lifecycle_diagnostics);
    std::vector<Diagnostic> result(
        begin,
        state_->diagnostics->lifecycle_diagnostics.end());
    state_->diagnostics->consumed_lifecycle_diagnostics =
        state_->diagnostics->lifecycle_diagnostics.size();
    return result;
}

std::expected<DiagnosticCursor, Diagnostic>
Device::diagnostic_cursor() const {
    if (!state_ || !state_->diagnostics) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Device::diagnostic_cursor called on an empty device",
        });
    }

    std::lock_guard lock(state_->diagnostics->mutex);
    return DiagnosticCursor{
        state_,
        state_->diagnostics->debug_messages.size(),
        state_->diagnostics->lifecycle_diagnostics.size(),
    };
}

std::expected<DiagnosticSnapshot, Diagnostic>
Device::diagnostics_since(const DiagnosticCursor& cursor) const {
    if (!state_ || !state_->diagnostics) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Device::diagnostics_since called on an empty device",
        });
    }

    const auto cursor_context = cursor.context_.lock();
    if (!cursor_context || cursor_context.get() != state_.get()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::incompatible_device,
            .message = "Diagnostic cursor belongs to a different or expired OpenGL context",
        });
    }

    std::lock_guard lock(state_->diagnostics->mutex);
    if (cursor.debug_message_index_ > state_->diagnostics->debug_messages.size()
        || cursor.lifecycle_diagnostic_index_
            > state_->diagnostics->lifecycle_diagnostics.size()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Diagnostic cursor is outside this context's retained history",
        });
    }

    const auto debug_begin = state_->diagnostics->debug_messages.begin()
        + static_cast<std::ptrdiff_t>(cursor.debug_message_index_);
    const auto lifecycle_begin = state_->diagnostics->lifecycle_diagnostics.begin()
        + static_cast<std::ptrdiff_t>(cursor.lifecycle_diagnostic_index_);
    return DiagnosticSnapshot{
        .debug_messages = std::vector<DebugMessage>(
            debug_begin,
            state_->diagnostics->debug_messages.end()),
        .lifecycle_diagnostics = std::vector<Diagnostic>(
            lifecycle_begin,
            state_->diagnostics->lifecycle_diagnostics.end()),
    };
}

std::expected<void, Diagnostic> Device::debug_marker(
    std::string_view message) const {
    if (message.empty() ||
        message.size() > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::debug_marker requires a non-empty GLsizei-sized message",
        });
    }
    if (auto current = require_current("Device::debug_marker"); !current) {
        return current;
    }
    glDebugMessageInsert(
        GL_DEBUG_SOURCE_APPLICATION,
        GL_DEBUG_TYPE_MARKER,
        0x564E47U,
        GL_DEBUG_SEVERITY_NOTIFICATION,
        static_cast<GLsizei>(message.size()),
        message.data());
    return {};
}

std::expected<void, Diagnostic> Device::viewport(
    std::int32_t x,
    std::int32_t y,
    std::int32_t width,
    std::int32_t height) const {
    if (width < 0 || height < 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::viewport requires non-negative dimensions",
        });
    }
    if (auto current = require_current("Device::viewport"); !current) {
        return current;
    }

    std::array<GLint, 2> maximum_dimensions{};
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_MAX_VIEWPORT_DIMS)",
            [&] {
                glGetIntegerv(
                    GL_MAX_VIEWPORT_DIMS,
                    maximum_dimensions.data());
            });
        !queried) {
        return queried;
    }
    if (maximum_dimensions[0] <= 0 || maximum_dimensions[1] <= 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported invalid GL_MAX_VIEWPORT_DIMS",
        });
    }
    if (width > maximum_dimensions[0] || height > maximum_dimensions[1]) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::viewport dimensions exceed GL_MAX_VIEWPORT_DIMS",
        });
    }

    const auto edge_is_representable = [](std::int32_t coordinate,
                                           std::int32_t extent) noexcept {
        const auto edge = static_cast<std::int64_t>(coordinate)
            + static_cast<std::int64_t>(extent);
        return edge >= static_cast<std::int64_t>(
                           std::numeric_limits<GLint>::lowest())
            && edge <= static_cast<std::int64_t>(
                           std::numeric_limits<GLint>::max());
    };
    if (!edge_is_representable(x, width)
        || !edge_is_representable(y, height)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::viewport coordinate plus extent is not representable by OpenGL",
        });
    }

    return detail::checked_gl_call(
        "glViewport",
        [&] { glViewport(x, y, width, height); });
}

std::expected<void, Diagnostic> Device::clear_default_color(
    const std::array<float, 4>& color) const {
    if (auto current = require_current("Device::clear_default_color"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "clear default framebuffer color",
        [&] {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // ClearBuffer does not modify GL_COLOR_CLEAR_VALUE. The full mask
            // makes the requested clear independent of ambient pipeline state
            // and establishes a deterministic beginning-of-frame baseline.
            glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearBufferfv(GL_COLOR, 0, color.data());
        });
}

std::expected<void, Diagnostic> Device::clear_default_depth(float depth) const {
    if (!std::isfinite(depth) || depth < 0.0F || depth > 1.0F) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::clear_default_depth requires a finite depth in [0, 1]",
        });
    }
    if (auto current = require_current("Device::clear_default_depth"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "clear default framebuffer depth",
        [&] {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // ClearBuffer does not modify GL_DEPTH_CLEAR_VALUE.
            glDepthMask(GL_TRUE);
            glClearBufferfv(GL_DEPTH, 0, &depth);
        });
}

std::expected<void, Diagnostic> Device::clear_default(
    const std::array<float, 4>& color,
    float depth) const {
    if (!std::isfinite(depth) || depth < 0.0F || depth > 1.0F) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::clear_default requires a finite depth in [0, 1]",
        });
    }
    if (auto current = require_current("Device::clear_default"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "clear default framebuffer color and depth",
        [&] {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glClearBufferfv(GL_COLOR, 0, color.data());
            glClearBufferfv(GL_DEPTH, 0, &depth);
        });
}

std::expected<void, Diagnostic> Device::set_pipeline_color_output_baseline(
    std::span<const std::uint32_t> color_attachments,
    bool all_color_attachments) const {
    if (auto current = require_current(
            "Device::set_pipeline_color_output_baseline");
        !current) {
        return current;
    }

    const auto maximum_draw_buffers = state_->maximum_draw_buffers;
    if (maximum_draw_buffers <= 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported an invalid GL_MAX_DRAW_BUFFERS while binding a graphics pipeline",
        });
    }
    if (!all_color_attachments) {
        for (const auto attachment : color_attachments) {
            if (attachment >= static_cast<std::uint32_t>(
                    maximum_draw_buffers)) {
                return std::unexpected(Diagnostic{
                    .code = ErrorCode::invalid_argument,
                    .message = "graphics-pipeline fragment output exceeds GL_MAX_DRAW_BUFFERS",
                });
            }
        }
    }

    return detail::checked_gl_call(
        "configure graphics-pipeline color outputs",
        [&] {
            const auto configure = [](GLuint attachment) {
                glDisablei(GL_BLEND, attachment);
                glColorMaski(
                    attachment,
                    GL_TRUE,
                    GL_TRUE,
                    GL_TRUE,
                    GL_TRUE);
            };

            if (all_color_attachments) {
                for (GLint attachment = 0;
                     attachment < maximum_draw_buffers;
                     ++attachment) {
                    configure(static_cast<GLuint>(attachment));
                }
                return;
            }
            for (const auto attachment : color_attachments) {
                configure(static_cast<GLuint>(attachment));
            }
        });
}

std::expected<void, Diagnostic> Device::set_depth_state(DepthState state) const {
    if (auto current = require_current("Device::set_depth_state"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "configure OpenGL depth state",
        [&] {
            if (state.test_enabled) {
                glEnable(GL_DEPTH_TEST);
            } else {
                glDisable(GL_DEPTH_TEST);
            }
            glDepthMask(state.write_enabled ? GL_TRUE : GL_FALSE);
            glDepthFunc(gl_depth_compare(state.compare));
        });
}

std::expected<void, Diagnostic> Device::set_blend_enabled(
    std::uint32_t color_attachment,
    bool enabled) const {
    if (auto current = require_current("Device::set_blend_enabled"); !current) {
        return current;
    }
    if (auto valid = validate_draw_buffer_index(
            color_attachment,
            "Device::set_blend_enabled",
            state_->maximum_draw_buffers);
        !valid) {
        return valid;
    }
    return detail::checked_gl_call(
        enabled ? "glEnablei(GL_BLEND)" : "glDisablei(GL_BLEND)",
        [&] {
            if (enabled) {
                glEnablei(GL_BLEND, color_attachment);
            } else {
                glDisablei(GL_BLEND, color_attachment);
            }
        });
}

std::expected<void, Diagnostic> Device::set_color_write_mask(
    std::uint32_t color_attachment,
    const std::array<bool, 4>& enabled) const {
    if (auto current = require_current("Device::set_color_write_mask"); !current) {
        return current;
    }
    if (auto valid = validate_draw_buffer_index(
            color_attachment,
            "Device::set_color_write_mask",
            state_->maximum_draw_buffers);
        !valid) {
        return valid;
    }
    return detail::checked_gl_call(
        "glColorMaski",
        [&] {
            glColorMaski(
                color_attachment,
                enabled[0] ? GL_TRUE : GL_FALSE,
                enabled[1] ? GL_TRUE : GL_FALSE,
                enabled[2] ? GL_TRUE : GL_FALSE,
                enabled[3] ? GL_TRUE : GL_FALSE);
        });
}

std::expected<void, Diagnostic> Device::set_scissor_enabled(bool enabled) const {
    if (auto current = require_current("Device::set_scissor_enabled"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        enabled ? "glEnable(GL_SCISSOR_TEST)" : "glDisable(GL_SCISSOR_TEST)",
        [&] { set_capability(GL_SCISSOR_TEST, enabled); });
}

std::expected<void, Diagnostic> Device::set_cull_state(CullState state) const {
    GLenum cull_face = GL_BACK;
    switch (state.mode) {
    case CullMode::none:
        break;
    case CullMode::front:
        cull_face = GL_FRONT;
        break;
    case CullMode::back:
        cull_face = GL_BACK;
        break;
    default:
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::set_cull_state received an invalid cull mode",
        });
    }

    GLenum front_face = GL_CCW;
    switch (state.front_face) {
    case FrontFaceWinding::clockwise:
        front_face = GL_CW;
        break;
    case FrontFaceWinding::counter_clockwise:
        front_face = GL_CCW;
        break;
    default:
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::set_cull_state received an invalid front-face winding",
        });
    }

    if (auto current = require_current("Device::set_cull_state"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "configure OpenGL cull state",
        [&] {
            set_capability(GL_CULL_FACE, state.mode != CullMode::none);
            if (state.mode != CullMode::none) {
                glCullFace(cull_face);
            }
            glFrontFace(front_face);
        });
}

std::expected<void, Diagnostic> Device::set_rasterizer_discard_enabled(
    bool enabled) const {
    if (auto current = require_current(
            "Device::set_rasterizer_discard_enabled");
        !current) {
        return current;
    }
    return detail::checked_gl_call(
        enabled
            ? "glEnable(GL_RASTERIZER_DISCARD)"
            : "glDisable(GL_RASTERIZER_DISCARD)",
        [&] { set_capability(GL_RASTERIZER_DISCARD, enabled); });
}

std::expected<void, Diagnostic> Device::set_framebuffer_srgb_enabled(
    bool enabled) const {
    if (auto current = require_current(
            "Device::set_framebuffer_srgb_enabled");
        !current) {
        return current;
    }
    return detail::checked_gl_call(
        enabled
            ? "glEnable(GL_FRAMEBUFFER_SRGB)"
            : "glDisable(GL_FRAMEBUFFER_SRGB)",
        [&] { set_capability(GL_FRAMEBUFFER_SRGB, enabled); });
}

std::expected<void, Diagnostic> Device::set_standard_raster_state() const {
    if (auto current = require_current("Device::set_standard_raster_state");
        !current) {
        return current;
    }

    GLint maximum_clip_distances = 0;
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_MAX_CLIP_DISTANCES)",
            [&] {
                glGetIntegerv(
                    GL_MAX_CLIP_DISTANCES,
                    &maximum_clip_distances);
            });
        !queried) {
        return queried;
    }
    if (maximum_clip_distances <= 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported an invalid GL_MAX_CLIP_DISTANCES",
        });
    }

    return detail::checked_gl_call(
        "configure standard OpenGL raster state",
        [&] {
            set_capability(GL_COLOR_LOGIC_OP, false);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

            // Multisampling itself remains owned by the render target. These
            // modifiers are disabled without overwriting their stored values.
            set_capability(GL_SAMPLE_MASK, false);
            set_capability(GL_SAMPLE_ALPHA_TO_COVERAGE, false);
            set_capability(GL_SAMPLE_ALPHA_TO_ONE, false);
            set_capability(GL_SAMPLE_COVERAGE, false);
            set_capability(GL_SAMPLE_SHADING, false);

            set_capability(GL_POLYGON_OFFSET_FILL, false);
            set_capability(GL_POLYGON_OFFSET_LINE, false);
            set_capability(GL_POLYGON_OFFSET_POINT, false);
            set_capability(GL_DEPTH_CLAMP, false);
            set_capability(GL_PRIMITIVE_RESTART, false);
            set_capability(GL_PRIMITIVE_RESTART_FIXED_INDEX, false);
            set_capability(GL_DITHER, false);
            set_capability(GL_POLYGON_SMOOTH, false);
            set_capability(GL_STENCIL_TEST, false);

            glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
            glDepthRange(0.0, 1.0);
            for (GLint index = 0; index < maximum_clip_distances; ++index) {
                set_capability(
                    GL_CLIP_DISTANCE0 + static_cast<GLenum>(index),
                    false);
            }
        });
}

std::expected<void, Diagnostic> Device::bind_default_framebuffer() const {
    if (auto current = require_current("Device::bind_default_framebuffer"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glBindFramebuffer(default)",
        [] { glBindFramebuffer(GL_FRAMEBUFFER, 0); });
}

std::expected<void, Diagnostic> Device::draw_arrays_instanced(
    Primitive primitive,
    std::uint32_t first,
    std::uint32_t vertex_count,
    std::uint32_t instance_count) const {
    if (first > static_cast<std::uint32_t>(std::numeric_limits<GLint>::max())
        || vertex_count > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())
        || instance_count > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::draw_arrays_instanced arguments exceed OpenGL integer ranges",
        });
    }
    if (auto current = require_current("Device::draw_arrays_instanced"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glDrawArraysInstanced",
        [&] {
            glDrawArraysInstanced(
                gl_primitive(primitive),
                static_cast<GLint>(first),
                static_cast<GLsizei>(vertex_count),
                static_cast<GLsizei>(instance_count));
        });
}

std::expected<void, Diagnostic> Device::draw_elements_instanced(
    Primitive primitive,
    IndexFormat index_format,
    std::uint32_t index_count,
    std::size_t index_byte_offset,
    std::uint32_t instance_count) const {
    std::size_t index_size = 0;
    GLenum gl_index_type = 0;
    switch (index_format) {
    case IndexFormat::u16:
        index_size = sizeof(std::uint16_t);
        gl_index_type = GL_UNSIGNED_SHORT;
        break;
    case IndexFormat::u32:
        index_size = sizeof(std::uint32_t);
        gl_index_type = GL_UNSIGNED_INT;
        break;
    default:
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::draw_elements_instanced received an invalid index format",
        });
    }
    const auto maximum_offset = static_cast<std::size_t>(
        std::numeric_limits<std::intptr_t>::max());
    if (index_count > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())
        || instance_count > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max())
        || index_byte_offset % index_size != 0
        || index_byte_offset > maximum_offset
        || static_cast<std::size_t>(index_count)
            > (maximum_offset - index_byte_offset) / index_size) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Device::draw_elements_instanced received an out-of-range count or a misaligned index offset",
        });
    }
    if (auto current = require_current("Device::draw_elements_instanced"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glDrawElementsInstanced",
        [&] {
            glDrawElementsInstanced(
                gl_primitive(primitive),
                static_cast<GLsizei>(index_count),
                gl_index_type,
                reinterpret_cast<const void*>(index_byte_offset),
                static_cast<GLsizei>(instance_count));
        });
}

std::expected<void, Diagnostic> Device::finish() const {
    if (auto current = require_current("Device::finish"); !current) {
        return current;
    }
    glFinish();
    return {};
}

} // namespace vng::opengl
