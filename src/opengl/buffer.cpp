#include <vng/opengl/buffer.hpp>

#include <vng/opengl/device.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <glad/gl.h>

#include <limits>
#include <utility>

namespace vng::opengl {
namespace {

[[nodiscard]] GLbitfield gl_storage_flags(BufferStorage storage) noexcept {
    GLbitfield result = 0;
    if (contains(storage, BufferStorage::dynamic)) {
        result |= GL_DYNAMIC_STORAGE_BIT;
    }
    if (contains(storage, BufferStorage::map_read)) {
        result |= GL_MAP_READ_BIT;
    }
    if (contains(storage, BufferStorage::map_write)) {
        result |= GL_MAP_WRITE_BIT;
    }
    return result;
}

[[nodiscard]] bool range_fits(
    std::size_t total,
    std::size_t offset,
    std::size_t length) noexcept {
    return offset <= total && length <= total - offset;
}

} // namespace

Buffer::Buffer(
    std::shared_ptr<detail::ContextState> state,
    std::uint32_t handle,
    std::size_t size,
    BufferStorage storage) noexcept
    : state_(std::move(state)),
      handle_(handle),
      size_(size),
      storage_(storage) {}

Buffer::Buffer(Buffer&& other) noexcept
    : state_(std::move(other.state_)),
      handle_(std::exchange(other.handle_, 0)),
      size_(std::exchange(other.size_, 0)),
      storage_(other.storage_) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        release_noexcept();
        state_ = std::move(other.state_);
        handle_ = std::exchange(other.handle_, 0);
        size_ = std::exchange(other.size_, 0);
        storage_ = other.storage_;
    }
    return *this;
}

Buffer::~Buffer() {
    release_noexcept();
}

bool Buffer::belongs_to(const Device& device) const noexcept {
    return handle_ != 0 && state_ && state_.get() == device.state_.get();
}

std::expected<Buffer, Diagnostic> Buffer::create(
    const Device& device,
    BufferCreateInfo info) {
    if (!device.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Buffer::create received an empty device",
        });
    }
    if (info.size == 0
        || info.size > static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Buffer::create requires a non-zero size representable by GLsizeiptr",
        });
    }
    if (!info.initial_data.empty() && info.initial_data.size() != info.size) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Buffer::create initial_data must be empty or cover the complete allocation",
        });
    }
    if (auto current = device.state_->require_current("Buffer::create"); !current) {
        return std::unexpected(std::move(current.error()));
    }

    GLuint handle = 0;
    if (auto created = detail::checked_gl_call(
            "glCreateBuffers",
            [&] { glCreateBuffers(1, &handle); },
            ErrorCode::object_creation_failed);
        !created) {
        if (handle != 0) {
            glDeleteBuffers(1, &handle);
            detail::clear_gl_errors();
        }
        return std::unexpected(std::move(created.error()));
    }
    if (handle == 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::object_creation_failed,
            .message = "glCreateBuffers returned zero",
        });
    }
    if (auto allocated = detail::checked_gl_call(
            "glNamedBufferStorage",
            [&] {
                glNamedBufferStorage(
                    handle,
                    static_cast<GLsizeiptr>(info.size),
                    info.initial_data.empty() ? nullptr : info.initial_data.data(),
                    gl_storage_flags(info.storage));
            });
        !allocated) {
        glDeleteBuffers(1, &handle);
        detail::clear_gl_errors();
        return std::unexpected(std::move(allocated.error()));
    }
    return Buffer(device.state_, handle, info.size, info.storage);
}

std::expected<Buffer, Diagnostic> Buffer::from_bytes(
    const Device& device,
    std::span<const std::byte> bytes,
    BufferStorage storage) {
    return create(device, BufferCreateInfo{
        .size = bytes.size(),
        .initial_data = bytes,
        .storage = storage,
    });
}

std::expected<void, Diagnostic> Buffer::write(
    std::size_t offset,
    std::span<const std::byte> bytes) {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Buffer::write called on an empty buffer",
        });
    }
    if (!contains(storage_, BufferStorage::dynamic)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Buffer::write requires BufferStorage::dynamic",
        });
    }
    if (!range_fits(size_, offset, bytes.size())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Buffer::write range exceeds the allocation",
        });
    }
    if (auto current = state_->require_current("Buffer::write"); !current) {
        return current;
    }
    if (!bytes.empty()) {
        glNamedBufferSubData(
            handle_,
            static_cast<GLintptr>(offset),
            static_cast<GLsizeiptr>(bytes.size()),
            bytes.data());
    }
    return {};
}

std::expected<void, Diagnostic> Buffer::read(
    std::size_t offset,
    std::span<std::byte> destination) const {
    if (handle_ == 0 || !state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Buffer::read called on an empty buffer",
        });
    }
    if (!range_fits(size_, offset, destination.size())) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "Buffer::read range exceeds the allocation",
        });
    }
    if (auto current = state_->require_current("Buffer::read"); !current) {
        return current;
    }
    if (!destination.empty()) {
        glGetNamedBufferSubData(
            handle_,
            static_cast<GLintptr>(offset),
            static_cast<GLsizeiptr>(destination.size()),
            destination.data());
    }
    return {};
}

std::expected<void, Diagnostic> Buffer::destroy() {
    if (handle_ == 0) {
        return {};
    }
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "Buffer::destroy has no device state",
        });
    }
    if (auto current = state_->require_current("Buffer::destroy"); !current) {
        return current;
    }
    const GLuint handle = handle_;
    glDeleteBuffers(1, &handle);
    handle_ = 0;
    size_ = 0;
    state_.reset();
    return {};
}

void Buffer::release_noexcept() noexcept {
    if (handle_ == 0) {
        return;
    }
    if (state_ && state_->is_current()) {
        const GLuint handle = handle_;
        glDeleteBuffers(1, &handle);
    } else if (state_) {
        state_->record_lifecycle_failure(
            "Buffer destroyed without its owning context current; native handle was leaked safely");
    }
    handle_ = 0;
    size_ = 0;
    state_.reset();
}

} // namespace vng::opengl
