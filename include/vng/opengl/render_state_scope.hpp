#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <vng/opengl/diagnostic.hpp>

namespace vng::opengl {

namespace detail { struct ContextState; }
class Device;

// Saves the context state that a contained render operation may replace and
// restores it at scope exit. The caller lists the fragment-output/draw-buffer
// indices whose indexed blend and color-mask state will be touched.
//
// Capturing state is intentionally explicit and comparatively heavyweight.
// The eventual renderer can instead own complete state, but tool/capture paths
// need a safe boundary while they coexist with caller-managed OpenGL.
class RenderStateScope final {
public:
    [[nodiscard]] static std::expected<RenderStateScope, Diagnostic> capture(
        const Device& device,
        std::span<const std::uint32_t> draw_buffers = {});

    RenderStateScope(RenderStateScope&& other) noexcept;
    RenderStateScope& operator=(RenderStateScope&& other) noexcept;
    RenderStateScope(const RenderStateScope&) = delete;
    RenderStateScope& operator=(const RenderStateScope&) = delete;
    ~RenderStateScope();

    // Restores once. Calling restore again after a successful or attempted GL
    // restore is a no-op. A wrong-context failure leaves the scope active so
    // the owner may make the context current and retry.
    [[nodiscard]] std::expected<void, Diagnostic> restore();
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    struct DrawBufferState final {
        std::uint32_t index{};
        bool blend_enabled{};
        std::array<bool, 4> color_write{};
    };

    struct ViewportState final {
        std::array<float, 4> viewport{};
        bool scissor_enabled{};
        std::array<std::int32_t, 4> scissor_box{};
        std::array<double, 2> depth_range{};
    };

    struct Snapshot final {
        std::uint32_t draw_framebuffer{};
        std::uint32_t read_framebuffer{};
        std::uint32_t program{};
        std::uint32_t vertex_array{};

        std::vector<ViewportState> viewports;

        bool depth_test_enabled{};
        bool depth_write_enabled{};
        std::uint32_t depth_compare{};

        std::vector<DrawBufferState> draw_buffers;

        bool cull_enabled{};
        std::uint32_t cull_mode{};
        std::uint32_t front_face{};

        bool rasterizer_discard_enabled{};
        bool framebuffer_srgb_enabled{};

        bool color_logic_op_enabled{};
        std::uint32_t polygon_mode{};

        bool sample_mask_enabled{};
        std::vector<std::uint32_t> sample_mask_words;
        bool sample_alpha_to_coverage_enabled{};
        bool sample_alpha_to_one_enabled{};
        bool sample_coverage_enabled{};
        float sample_coverage_value{};
        bool sample_coverage_inverted{};
        bool sample_shading_enabled{};

        bool polygon_offset_fill_enabled{};
        bool polygon_offset_line_enabled{};
        bool polygon_offset_point_enabled{};
        bool depth_clamp_enabled{};
        bool primitive_restart_enabled{};
        bool primitive_restart_fixed_index_enabled{};
        bool dither_enabled{};
        bool polygon_smooth_enabled{};
        bool stencil_test_enabled{};

        std::uint32_t clip_origin{};
        std::uint32_t clip_depth_mode{};
        std::vector<std::uint8_t> clip_distance_enabled;
    };

    RenderStateScope(
        std::shared_ptr<detail::ContextState> state,
        Snapshot snapshot) noexcept;

    void restore_noexcept() noexcept;

    std::shared_ptr<detail::ContextState> state_;
    Snapshot snapshot_;
    bool active_{true};
};

} // namespace vng::opengl
