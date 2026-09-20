#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <vng/opengl/context_access.hpp>
#include <vng/opengl/backend.hpp>
#include <vng/opengl/default_framebuffer.hpp>
#include <vng/opengl/diagnostic.hpp>

namespace vng::opengl {

namespace detail { struct ContextState; class GraphicsStateAccess; }

class Shader;
class Program;
class Buffer;
class VertexArray;
class Renderbuffer;
class Framebuffer;
class Image2D;
class RenderStateScope;
class Frame;
class Commands;

// A cheap bookmark into the append-only diagnostic history of one OpenGL
// context. It is copyable, does not keep the context alive, and works across
// Device facades that represent that same context.
class DiagnosticCursor final {
public:
    DiagnosticCursor(const DiagnosticCursor&) noexcept = default;
    DiagnosticCursor& operator=(const DiagnosticCursor&) noexcept = default;
    DiagnosticCursor(DiagnosticCursor&&) noexcept = default;
    DiagnosticCursor& operator=(DiagnosticCursor&&) noexcept = default;

private:
    DiagnosticCursor(
        std::weak_ptr<detail::ContextState> context,
        std::size_t debug_message_index,
        std::size_t lifecycle_diagnostic_index) noexcept
        : context_(std::move(context)),
          debug_message_index_(debug_message_index),
          lifecycle_diagnostic_index_(lifecycle_diagnostic_index) {}

    std::weak_ptr<detail::ContextState> context_;
    std::size_t debug_message_index_{};
    std::size_t lifecycle_diagnostic_index_{};

    friend class Device;
};

struct DiagnosticSnapshot final {
    std::vector<DebugMessage> debug_messages;
    std::vector<Diagnostic> lifecycle_diagnostics;

    [[nodiscard]] bool empty() const noexcept {
        return debug_messages.empty() && lifecycle_diagnostics.empty();
    }
};

struct DeviceOptions final {
    int required_major{4};
    int required_minor{6};
    bool enable_debug_output{true};
    bool synchronous_debug_output{true};
};

enum class Primitive {
    points,
    lines,
    line_strip,
    triangles,
    triangle_strip,
};

enum class IndexFormat {
    u16,
    u32,
};

enum class DepthCompare {
    never,
    less,
    less_equal,
    equal,
    greater_equal,
    greater,
    not_equal,
    always,
};

struct DepthState final {
    bool test_enabled{true};
    bool write_enabled{true};
    DepthCompare compare{DepthCompare::less};
};

enum class CullMode {
    none,
    front,
    back,
};

enum class FrontFaceWinding {
    clockwise,
    counter_clockwise,
};

struct CullState final {
    CullMode mode{CullMode::none};
    FrontFaceWinding front_face{FrontFaceWinding::counter_clockwise};
};

class Device final {
public:
    using backend_type = Backend;
    static std::expected<Device, Diagnostic> create(
        CurrentContextAccess access,
        DeviceOptions options = {});

    Device(Device&&) noexcept = default;
    Device& operator=(Device&&) noexcept = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

    [[nodiscard]] int major_version() const noexcept;
    [[nodiscard]] int minor_version() const noexcept;
    [[nodiscard]] DefaultFramebufferCapabilities
    default_framebuffer_capabilities() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] std::expected<void, Diagnostic> require_current(
        std::string_view operation = "OpenGL operation") const;
    // Resource replacement must not invalidate a live frame's borrowed
    // programs, attachments, or vertex-input state.
    [[nodiscard]] std::expected<void, Diagnostic> require_resource_update(
        std::string_view operation = "resource update") const;

    [[nodiscard]] std::vector<DebugMessage> take_debug_messages() const;
    [[nodiscard]] std::vector<Diagnostic> take_lifecycle_diagnostics() const;

    // Marks both diagnostic streams at one context-local point. Snapshot reads
    // are non-destructive and independent of the legacy take_* consumers.
    [[nodiscard]] std::expected<DiagnosticCursor, Diagnostic>
    diagnostic_cursor() const;

    [[nodiscard]] std::expected<DiagnosticSnapshot, Diagnostic>
    diagnostics_since(const DiagnosticCursor& cursor) const;

    [[nodiscard]] std::expected<void, Diagnostic> debug_marker(
        std::string_view message) const;

    [[nodiscard]] std::expected<void, Diagnostic> viewport(
        std::int32_t x,
        std::int32_t y,
        std::int32_t width,
        std::int32_t height) const;

    [[nodiscard]] std::expected<void, Diagnostic> clear_default_color(
        const std::array<float, 4>& color) const;

    [[nodiscard]] std::expected<void, Diagnostic> clear_default_depth(
        float depth) const;

    // Default-target clears establish full write masks for each attachment
    // they clear and preserve OpenGL's ambient clear-value state. This makes
    // beginning a frame independent of the previous draw state.
    // Clearing both together is the usual beginning-of-frame operation for a
    // depth-tested render path; the default depth value accepts every GL_LESS
    // fragment in the conventional depth configuration.
    [[nodiscard]] std::expected<void, Diagnostic> clear_default(
        const std::array<float, 4>& color,
        float depth = 1.0F) const;

    // Render paths establish the state they own explicitly. This avoids
    // analysis rendering depending on whatever a host happened to leave in
    // OpenGL's ambient state.
    [[nodiscard]] std::expected<void, Diagnostic> set_depth_state(
        DepthState state) const;

    [[nodiscard]] std::expected<void, Diagnostic> set_blend_enabled(
        std::uint32_t color_attachment,
        bool enabled) const;

    [[nodiscard]] std::expected<void, Diagnostic> set_color_write_mask(
        std::uint32_t color_attachment,
        const std::array<bool, 4>& enabled) const;

    [[nodiscard]] std::expected<void, Diagnostic> set_scissor_enabled(
        bool enabled) const;

    [[nodiscard]] std::expected<void, Diagnostic> set_cull_state(
        CullState state) const;

    [[nodiscard]] std::expected<void, Diagnostic>
    set_rasterizer_discard_enabled(bool enabled) const;

    [[nodiscard]] std::expected<void, Diagnostic>
    set_framebuffer_srgb_enabled(bool enabled) const;

    // Establishes the fixed-function raster baseline shared by ordinary and
    // inspection rendering. State configured by the dedicated depth, cull,
    // blend, scissor, and framebuffer-sRGB setters remains separate.
    [[nodiscard]] std::expected<void, Diagnostic>
    set_standard_raster_state() const;

    [[nodiscard]] std::expected<void, Diagnostic> bind_default_framebuffer() const;

    [[nodiscard]] std::expected<void, Diagnostic> draw_arrays_instanced(
        Primitive primitive,
        std::uint32_t first,
        std::uint32_t vertex_count,
        std::uint32_t instance_count = 1) const;

    // Advanced stateful draw primitive. The caller must bind a VAO whose
    // element buffer covers the complete byte range; GpuMesh provides the
    // checked ownership path for ordinary mesh rendering.
    [[nodiscard]] std::expected<void, Diagnostic> draw_elements_instanced(
        Primitive primitive,
        IndexFormat index_format,
        std::uint32_t index_count,
        std::size_t index_byte_offset = 0,
        std::uint32_t instance_count = 1) const;

    [[nodiscard]] std::expected<void, Diagnostic> finish() const;

private:
    explicit Device(std::shared_ptr<detail::ContextState> state) noexcept;

    // Establish deterministic per-attachment write/blend defaults.
    [[nodiscard]] std::expected<void, Diagnostic>
    reset_color_outputs() const;

    std::shared_ptr<detail::ContextState> state_;

    friend class Shader;
    friend class Program;
    friend class Buffer;
    friend class VertexArray;
    friend class Renderbuffer;
    friend class Framebuffer;
    friend class Rgba8ReadbackQueue;
    friend class Image2D;
    friend class RenderStateScope;
    friend class Frame;
    friend class Commands;
    friend class detail::GraphicsStateAccess;
};

} // namespace vng::opengl
