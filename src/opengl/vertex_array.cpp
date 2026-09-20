#include <vng/opengl/vertex_array.hpp>

#include <vng/opengl/buffer.hpp>
#include <vng/opengl/device.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace vng::opengl {
namespace {

[[nodiscard]] bool is_packed(VertexComponent component) noexcept {
    return component == VertexComponent::sint_2_10_10_10_rev
        || component == VertexComponent::uint_2_10_10_10_rev;
}

[[nodiscard]] bool is_integral(VertexComponent component) noexcept {
    return component != VertexComponent::float32
        && component != VertexComponent::float16;
}

[[nodiscard]] GLenum gl_component(VertexComponent component) noexcept {
    switch (component) {
    case VertexComponent::float32: return GL_FLOAT;
    case VertexComponent::float16: return GL_HALF_FLOAT;
    case VertexComponent::sint32: return GL_INT;
    case VertexComponent::uint32: return GL_UNSIGNED_INT;
    case VertexComponent::sint16: return GL_SHORT;
    case VertexComponent::uint16: return GL_UNSIGNED_SHORT;
    case VertexComponent::sint8: return GL_BYTE;
    case VertexComponent::uint8: return GL_UNSIGNED_BYTE;
    case VertexComponent::sint_2_10_10_10_rev: return GL_INT_2_10_10_10_REV;
    case VertexComponent::uint_2_10_10_10_rev: return GL_UNSIGNED_INT_2_10_10_10_REV;
    }
    return GL_FLOAT;
}

[[nodiscard]] std::expected<void, Diagnostic> validate_format(VertexFormat format) {
    if (format.component_count < 1 || format.component_count > 4) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Vertex attribute component_count must be between one and four",
        });
    }
    if (is_packed(format.component) && format.component_count != 3
        && format.component_count != 4) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Packed 2:10:10:10 attributes require three or four logical components",
        });
    }
    if (format.delivery == VertexDelivery::integer) {
        if (!is_integral(format.component) || is_packed(format.component)) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "Integer vertex delivery requires a non-packed integer component type",
            });
        }
        if (format.normalized) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "Integer vertex delivery cannot also request normalization",
            });
        }
    }
    if ((format.component == VertexComponent::float32
            || format.component == VertexComponent::float16)
        && format.normalized) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Floating-point vertex storage cannot request normalization",
        });
    }
    return {};
}

} // namespace

VertexArray::VertexArray(
    std::shared_ptr<detail::ContextState> state,
    std::uint32_t handle) noexcept
    : state_(std::move(state)), handle_(handle) {}

VertexArray::VertexArray(VertexArray&& other) noexcept
    : state_(std::move(other.state_)),
      handle_(std::exchange(other.handle_, 0)),
      enabled_locations_(std::move(other.enabled_locations_)) {}

VertexArray& VertexArray::operator=(VertexArray&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, 0);
        enabled_locations_ = std::move(other.enabled_locations_);
    }
    return *this;
}

VertexArray::~VertexArray() {
    release_noexcept();
}

std::expected<VertexArray, Diagnostic> VertexArray::create(const Device& device) {
    if (!device.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "VertexArray::create received an empty device",
        });
    }
    if (auto current = device.state_->require_current("VertexArray::create"); !current) {
        return std::unexpected(std::move(current.error()));
    }
    GLuint handle = 0;
    if (auto created = detail::checked_gl_call(
            "glCreateVertexArrays",
            [&] { glCreateVertexArrays(1, &handle); },
            ErrorCode::object_creation_failed);
        !created) {
        if (handle != 0) {
            glDeleteVertexArrays(1, &handle);
            detail::clear_gl_errors();
        }
        return std::unexpected(std::move(created.error()));
    }
    if (handle == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::object_creation_failed,
            .message = "glCreateVertexArrays returned zero",
        });
    }
    return VertexArray(device.state_, handle);
}

std::expected<void, Diagnostic> VertexArray::configure(
    std::span<const VertexBufferBinding> bindings,
    std::span<const VertexAttribute> attributes) {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "VertexArray::configure called on an empty vertex array",
        });
    }
    if (auto current = state_->require_current("VertexArray::configure"); !current) {
        return current;
    }

    GLint max_attributes = 0;
    GLint max_bindings = 0;
    GLint max_relative_offset = 0;
    GLint max_stride = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attributes);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIB_BINDINGS, &max_bindings);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET, &max_relative_offset);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIB_STRIDE, &max_stride);

    std::unordered_set<std::uint32_t> binding_numbers;
    for (const auto& binding : bindings) {
        if (binding.buffer == nullptr || binding.buffer->handle_ == 0) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "VertexArray::configure received a null or empty vertex buffer",
            });
        }
        if (binding.buffer->state_.get() != state_.get()) {
            return std::unexpected(detail::incompatible_device("Vertex buffer"));
        }
        if (!binding_numbers.insert(binding.binding).second) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "VertexArray::configure received a duplicate binding number",
            });
        }
        if (binding.binding >= static_cast<std::uint32_t>(max_bindings)
            || binding.stride == 0
            || binding.stride > static_cast<std::uint32_t>(max_stride)
            || binding.offset > binding.buffer->size_
            || binding.offset > static_cast<std::size_t>(std::numeric_limits<GLintptr>::max())) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "VertexArray::configure received an out-of-range binding descriptor",
            });
        }
    }

    std::unordered_set<std::uint32_t> locations;
    for (const auto& attribute : attributes) {
        if (!locations.insert(attribute.location).second) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "VertexArray::configure received a duplicate attribute location",
            });
        }
        if (attribute.location >= static_cast<std::uint32_t>(max_attributes)
            || attribute.relative_offset > static_cast<std::uint32_t>(max_relative_offset)
            || !binding_numbers.contains(attribute.binding)) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "VertexArray::configure received an out-of-range attribute descriptor",
            });
        }
        if (auto valid = validate_format(attribute.format); !valid) {
            return valid;
        }
    }

    std::vector<std::uint32_t> new_locations;
    new_locations.reserve(attributes.size());
    for (const auto& attribute : attributes) {
        new_locations.push_back(attribute.location);
    }

    auto configured = detail::checked_gl_call(
        "OpenGL vertex-array configuration",
        [&] {
            for (const auto location : enabled_locations_) {
                glDisableVertexArrayAttrib(handle_, location);
            }

            for (const auto& binding : bindings) {
                glVertexArrayVertexBuffer(
                    handle_,
                    binding.binding,
                    binding.buffer->handle_,
                    static_cast<GLintptr>(binding.offset),
                    static_cast<GLsizei>(binding.stride));
                glVertexArrayBindingDivisor(handle_, binding.binding, binding.divisor);
            }

            for (const auto& attribute : attributes) {
                const auto component_count = is_packed(attribute.format.component)
                    ? 4
                    : static_cast<GLint>(attribute.format.component_count);
                if (attribute.format.delivery == VertexDelivery::integer) {
                    glVertexArrayAttribIFormat(
                        handle_,
                        attribute.location,
                        component_count,
                        gl_component(attribute.format.component),
                        attribute.relative_offset);
                } else {
                    glVertexArrayAttribFormat(
                        handle_,
                        attribute.location,
                        component_count,
                        gl_component(attribute.format.component),
                        attribute.format.normalized ? GL_TRUE : GL_FALSE,
                        attribute.relative_offset);
                }
                glVertexArrayAttribBinding(
                    handle_, attribute.location, attribute.binding);
                glEnableVertexArrayAttrib(handle_, attribute.location);
            }
        });

    if (configured) {
        enabled_locations_ = std::move(new_locations);
        return {};
    }

    // A GL error may leave a partially configured VAO. Retain the union so a
    // later successful configure disables every location that may be enabled.
    for (const auto location : new_locations) {
        if (!std::ranges::contains(enabled_locations_, location)) {
            enabled_locations_.push_back(location);
        }
    }
    return configured;
}

std::expected<void, Diagnostic> VertexArray::set_element_buffer(
    const Buffer& buffer) {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "VertexArray::set_element_buffer called on an empty vertex array",
        });
    }
    if (buffer.handle_ == 0 || !buffer.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "VertexArray::set_element_buffer received an empty buffer",
        });
    }
    if (buffer.state_.get() != state_.get()) {
        return std::unexpected(detail::incompatible_device("Element buffer"));
    }
    if (auto current = state_->require_current("VertexArray::set_element_buffer"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glVertexArrayElementBuffer",
        [&] { glVertexArrayElementBuffer(handle_, buffer.handle_); });
}

std::expected<void, Diagnostic> VertexArray::set_vertex_buffer(
    std::uint32_t binding, const Buffer& buffer, std::size_t offset, std::uint32_t stride) {
    if (!handle_ || !state_ || !buffer.handle_ || !buffer.state_ ||
        offset > buffer.size_ || offset > static_cast<std::size_t>(std::numeric_limits<GLintptr>::max()) ||
        !stride || stride > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max()))
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "VertexArray::set_vertex_buffer requires live storage and representable offset/stride"});
    if (state_.get() != buffer.state_.get())
        return std::unexpected(detail::incompatible_device("Vertex buffer"));
    if (auto current = state_->require_current("VertexArray::set_vertex_buffer"); !current) return current;
    return detail::checked_gl_call("glVertexArrayVertexBuffer", [&] {
        glVertexArrayVertexBuffer(handle_, binding, buffer.handle_,
            static_cast<GLintptr>(offset), static_cast<GLsizei>(stride));
    });
}

std::expected<void, Diagnostic> VertexArray::bind() const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "VertexArray::bind called on an empty vertex array",
        });
    }
    if (auto current = state_->require_current("VertexArray::bind"); !current) {
        return current;
    }
    return detail::checked_gl_call(
        "glBindVertexArray",
        [&] { glBindVertexArray(handle_); });
}

std::expected<void, Diagnostic> VertexArray::destroy() {
    if (handle_ == 0) {
        return {};
    }
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "VertexArray::destroy has no device state",
        });
    }
    if (auto current = state_->require_current("VertexArray::destroy"); !current) {
        return current;
    }
    const GLuint handle = handle_;
    glDeleteVertexArrays(1, &handle);
    handle_ = 0;
    enabled_locations_.clear();
    state_.reset();
    return {};
}

void VertexArray::release_noexcept() noexcept {
    if (handle_ == 0) {
        return;
    }
    if (state_ && state_->is_current()) {
        const GLuint handle = handle_;
        glDeleteVertexArrays(1, &handle);
    } else if (state_) {
        state_->record_lifecycle_failure(
            "VertexArray destroyed without its owning context current; native handle was leaked safely");
    }
    handle_ = 0;
    enabled_locations_.clear();
    state_.reset();
}

} // namespace vng::opengl
