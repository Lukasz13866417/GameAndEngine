#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <vng/core/types.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/shader.hpp>

namespace vng::opengl {

namespace detail { struct ContextState; }
class Device;

class Program final {
public:
    static std::expected<Program, Diagnostic> link(
        const Device& device,
        std::span<const Shader* const> shaders);

    static std::expected<Program, Diagnostic> link_graphics(
        const Device& device,
        const Shader& vertex,
        const Shader& fragment);

    Program(Program&& other) noexcept;
    Program& operator=(Program&& other) noexcept;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    ~Program();

    [[nodiscard]] std::uint32_t native_handle() const noexcept { return handle_; }
    [[nodiscard]] const std::string& driver_log() const noexcept { return driver_log_; }
    [[nodiscard]] bool belongs_to(const Device& device) const noexcept;
    [[nodiscard]] const std::optional<std::vector<VertexInputMetadata>>&
    vertex_inputs() const noexcept { return vertex_inputs_; }
    [[nodiscard]] std::expected<void, Diagnostic> bind() const;
    [[nodiscard]] std::expected<void, Diagnostic> set_uniform_u32(
        std::uint32_t location,
        std::uint32_t value) const;
    [[nodiscard]] std::expected<void, Diagnostic> set_uniform_mat4(
        std::uint32_t location,
        const Mat4& value) const;
    [[nodiscard]] std::expected<void, Diagnostic> destroy();

private:
    Program(
        std::shared_ptr<detail::ContextState> state,
        std::uint32_t handle,
        std::string driver_log,
        std::optional<std::vector<VertexInputMetadata>> vertex_inputs) noexcept;
    void release_noexcept() noexcept;

    std::shared_ptr<detail::ContextState> state_;
    std::uint32_t handle_{};
    std::string driver_log_;
    std::optional<std::vector<VertexInputMetadata>> vertex_inputs_;
};

} // namespace vng::opengl
