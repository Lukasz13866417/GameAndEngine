#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include <vng/opengl/diagnostic.hpp>

namespace vng::opengl {

namespace detail { struct ContextState; }
class Device;
class VertexArray;

enum class BufferStorage : std::uint32_t {
    none = 0,
    dynamic = 1u << 0,
    map_read = 1u << 1,
    map_write = 1u << 2,
};

[[nodiscard]] constexpr BufferStorage operator|(
    BufferStorage lhs,
    BufferStorage rhs) noexcept {
    return static_cast<BufferStorage>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr bool contains(
    BufferStorage value,
    BufferStorage bit) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(bit)) != 0;
}

struct BufferCreateInfo final {
    std::size_t size{};
    std::span<const std::byte> initial_data{};
    BufferStorage storage{BufferStorage::none};
};

class Buffer final {
public:
    static std::expected<Buffer, Diagnostic> create(
        const Device& device,
        BufferCreateInfo info);

    static std::expected<Buffer, Diagnostic> from_bytes(
        const Device& device,
        std::span<const std::byte> bytes,
        BufferStorage storage = BufferStorage::none);

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer();

    [[nodiscard]] std::uint32_t native_handle() const noexcept { return handle_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool belongs_to(const Device& device) const noexcept;

    [[nodiscard]] std::expected<void, Diagnostic> write(
        std::size_t offset,
        std::span<const std::byte> bytes);

    [[nodiscard]] std::expected<void, Diagnostic> read(
        std::size_t offset,
        std::span<std::byte> destination) const;

    [[nodiscard]] std::expected<void, Diagnostic> destroy();

private:
    Buffer(
        std::shared_ptr<detail::ContextState> state,
        std::uint32_t handle,
        std::size_t size,
        BufferStorage storage) noexcept;
    void release_noexcept() noexcept;

    std::shared_ptr<detail::ContextState> state_;
    std::uint32_t handle_{};
    std::size_t size_{};
    BufferStorage storage_{BufferStorage::none};

    friend class VertexArray;
};

} // namespace vng::opengl
