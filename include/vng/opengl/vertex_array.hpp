#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <vng/opengl/diagnostic.hpp>

namespace vng::opengl {

namespace detail { struct ContextState; }
class Buffer;
class Device;

enum class VertexComponent {
    float32,
    float16,
    sint32,
    uint32,
    sint16,
    uint16,
    sint8,
    uint8,
    sint_2_10_10_10_rev,
    uint_2_10_10_10_rev,
};

enum class VertexDelivery { floating, integer };

struct VertexFormat final {
    VertexComponent component{VertexComponent::float32};
    std::uint8_t component_count{1};
    bool normalized{false};
    VertexDelivery delivery{VertexDelivery::floating};
};

struct VertexBufferBinding final {
    std::uint32_t binding{};
    const Buffer* buffer{};
    std::size_t offset{};
    std::uint32_t stride{};
    std::uint32_t divisor{};
};

struct VertexAttribute final {
    std::uint32_t location{};
    std::uint32_t binding{};
    std::uint32_t relative_offset{};
    VertexFormat format{};
};

class VertexArray final {
public:
    static std::expected<VertexArray, Diagnostic> create(const Device& device);

    VertexArray(VertexArray&& other) noexcept;
    VertexArray& operator=(VertexArray&& other) noexcept;
    VertexArray(const VertexArray&) = delete;
    VertexArray& operator=(const VertexArray&) = delete;
    ~VertexArray();

    [[nodiscard]] std::uint32_t native_handle() const noexcept { return handle_; }

    [[nodiscard]] std::expected<void, Diagnostic> configure(
        std::span<const VertexBufferBinding> bindings,
        std::span<const VertexAttribute> attributes);

    [[nodiscard]] std::expected<void, Diagnostic> set_element_buffer(
        const Buffer& buffer);

    // Rebind storage without changing attribute formats or the binding divisor.
    [[nodiscard]] std::expected<void, Diagnostic> set_vertex_buffer(
        std::uint32_t binding, const Buffer& buffer, std::size_t offset,
        std::uint32_t stride);

    [[nodiscard]] std::expected<void, Diagnostic> bind() const;
    [[nodiscard]] std::expected<void, Diagnostic> destroy();

private:
    VertexArray(
        std::shared_ptr<detail::ContextState> state,
        std::uint32_t handle) noexcept;
    void release_noexcept() noexcept;

    std::shared_ptr<detail::ContextState> state_;
    std::uint32_t handle_{};
    std::vector<std::uint32_t> enabled_locations_;
};

} // namespace vng::opengl
