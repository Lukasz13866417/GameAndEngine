#include <vng/opengl/render_state_scope.hpp>

#include <vng/opengl/device.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace vng::opengl {
namespace {

void set_capability(GLenum capability, bool enabled) noexcept
{
    if (enabled) {
        glEnable(capability);
    } else {
        glDisable(capability);
    }
}

void set_indexed_capability(
    GLenum capability,
    GLuint index,
    bool enabled) noexcept
{
    if (enabled) {
        glEnablei(capability, index);
    } else {
        glDisablei(capability, index);
    }
}

[[nodiscard]] bool enabled(GLenum capability) noexcept
{
    return glIsEnabled(capability) == GL_TRUE;
}

[[nodiscard]] bool enabled(GLenum capability, GLuint index) noexcept
{
    return glIsEnabledi(capability, index) == GL_TRUE;
}

} // namespace

RenderStateScope::RenderStateScope(
    std::shared_ptr<detail::ContextState> state,
    Snapshot snapshot) noexcept
    : state_(std::move(state)), snapshot_(std::move(snapshot)) {}

RenderStateScope::RenderStateScope(RenderStateScope&& other) noexcept
    : state_(std::move(other.state_)),
      snapshot_(std::move(other.snapshot_)),
      active_(std::exchange(other.active_, false)) {}

RenderStateScope& RenderStateScope::operator=(RenderStateScope&& other) noexcept
{
    if (this != &other) {
        restore_noexcept();
        state_ = std::move(other.state_);
        snapshot_ = std::move(other.snapshot_);
        active_ = std::exchange(other.active_, false);
    }
    return *this;
}

RenderStateScope::~RenderStateScope()
{
    restore_noexcept();
}

std::expected<RenderStateScope, Diagnostic> RenderStateScope::capture(
    const Device& device,
    std::span<const std::uint32_t> draw_buffers)
{
    if (!device.state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "RenderStateScope::capture received an empty device",
        });
    }
    if (auto current = device.state_->require_current(
            "RenderStateScope::capture");
        !current) {
        return std::unexpected(std::move(current.error()));
    }

    GLint maximum_draw_buffers = 0;
    if (auto queried = detail::checked_gl_call(
            "glGetIntegerv(GL_MAX_DRAW_BUFFERS)",
            [&] {
                glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maximum_draw_buffers);
            });
        !queried) {
        return std::unexpected(std::move(queried.error()));
    }
    if (maximum_draw_buffers <= 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported an invalid GL_MAX_DRAW_BUFFERS",
        });
    }

    GLint maximum_sample_mask_words = 0;
    GLint maximum_clip_distances = 0;
    GLint maximum_viewports = 0;
    if (auto queried = detail::checked_gl_call(
            "capture OpenGL raster-state limits",
            [&] {
                glGetIntegerv(
                    GL_MAX_SAMPLE_MASK_WORDS,
                    &maximum_sample_mask_words);
                glGetIntegerv(
                    GL_MAX_CLIP_DISTANCES,
                    &maximum_clip_distances);
                glGetIntegerv(GL_MAX_VIEWPORTS, &maximum_viewports);
            });
        !queried) {
        return std::unexpected(std::move(queried.error()));
    }
    if (maximum_sample_mask_words <= 0
        || maximum_clip_distances <= 0
        || maximum_viewports <= 0) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::operation_failed,
            .message = "OpenGL reported invalid raster-state implementation limits",
        });
    }

    std::vector<std::uint32_t> indices(draw_buffers.begin(), draw_buffers.end());
    std::ranges::sort(indices);
    const auto unique_end = std::ranges::unique(indices).begin();
    indices.erase(unique_end, indices.end());
    for (const auto index : indices) {
        if (index >= static_cast<std::uint32_t>(maximum_draw_buffers)) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_argument,
                .message = "RenderStateScope draw-buffer index exceeds GL_MAX_DRAW_BUFFERS",
            });
        }
    }

    Snapshot snapshot;
    snapshot.draw_buffers.reserve(indices.size());
    for (const auto index : indices) {
        snapshot.draw_buffers.push_back(DrawBufferState{.index = index});
    }
    snapshot.sample_mask_words.resize(
        static_cast<std::size_t>(maximum_sample_mask_words));
    snapshot.clip_distance_enabled.resize(
        static_cast<std::size_t>(maximum_clip_distances));
    snapshot.viewports.resize(static_cast<std::size_t>(maximum_viewports));

    GLint draw_framebuffer = 0;
    GLint read_framebuffer = 0;
    GLint program = 0;
    GLint vertex_array = 0;
    GLint depth_compare = 0;
    GLint cull_mode = 0;
    GLint front_face = 0;
    // GL 4.6 core exposes one shared mode. Two elements keep this query safe
    // with implementations that retain the legacy front/back query width.
    std::array<GLint, 2> polygon_mode{};
    GLint clip_origin = 0;
    GLint clip_depth_mode = 0;
    GLboolean depth_write = GL_FALSE;
    GLboolean sample_coverage_inverted = GL_FALSE;

    if (auto queried = detail::checked_gl_call(
            "capture OpenGL render state",
            [&] {
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
                glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);

                for (std::size_t index = 0;
                     index < snapshot.viewports.size();
                     ++index) {
                    auto& viewport = snapshot.viewports[index];
                    const auto gl_index = static_cast<GLuint>(index);
                    glGetFloati_v(
                        GL_VIEWPORT,
                        gl_index,
                        viewport.viewport.data());
                    viewport.scissor_enabled = enabled(
                        GL_SCISSOR_TEST,
                        gl_index);
                    glGetIntegeri_v(
                        GL_SCISSOR_BOX,
                        gl_index,
                        viewport.scissor_box.data());
                    glGetDoublei_v(
                        GL_DEPTH_RANGE,
                        gl_index,
                        viewport.depth_range.data());
                }

                snapshot.depth_test_enabled = enabled(GL_DEPTH_TEST);
                glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write);
                glGetIntegerv(GL_DEPTH_FUNC, &depth_compare);

                for (auto& draw_buffer : snapshot.draw_buffers) {
                    draw_buffer.blend_enabled = enabled(
                        GL_BLEND, draw_buffer.index);
                    std::array<GLint, 4> blend_factors{};
                    glGetIntegeri_v(GL_BLEND_SRC_RGB, draw_buffer.index,
                                    &blend_factors[0]);
                    glGetIntegeri_v(GL_BLEND_DST_RGB, draw_buffer.index,
                                    &blend_factors[1]);
                    glGetIntegeri_v(GL_BLEND_SRC_ALPHA, draw_buffer.index,
                                    &blend_factors[2]);
                    glGetIntegeri_v(GL_BLEND_DST_ALPHA, draw_buffer.index,
                                    &blend_factors[3]);
                    std::ranges::transform(
                        blend_factors, draw_buffer.blend_factors.begin(),
                        [](GLint value) { return static_cast<std::uint32_t>(value); });
                    std::array<GLint, 2> blend_equations{};
                    glGetIntegeri_v(GL_BLEND_EQUATION_RGB, draw_buffer.index,
                                    &blend_equations[0]);
                    glGetIntegeri_v(GL_BLEND_EQUATION_ALPHA, draw_buffer.index,
                                    &blend_equations[1]);
                    std::ranges::transform(
                        blend_equations, draw_buffer.blend_equations.begin(),
                        [](GLint value) { return static_cast<std::uint32_t>(value); });
                    std::array<GLboolean, 4> write_mask{};
                    glGetBooleani_v(
                        GL_COLOR_WRITEMASK,
                        draw_buffer.index,
                        write_mask.data());
                    std::ranges::transform(
                        write_mask,
                        draw_buffer.color_write.begin(),
                        [](GLboolean value) { return value == GL_TRUE; });
                }

                snapshot.cull_enabled = enabled(GL_CULL_FACE);
                glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);
                glGetIntegerv(GL_FRONT_FACE, &front_face);

                snapshot.rasterizer_discard_enabled = enabled(
                    GL_RASTERIZER_DISCARD);
                snapshot.framebuffer_srgb_enabled = enabled(
                    GL_FRAMEBUFFER_SRGB);

                snapshot.color_logic_op_enabled = enabled(
                    GL_COLOR_LOGIC_OP);
                glGetIntegerv(GL_POLYGON_MODE, polygon_mode.data());

                snapshot.sample_mask_enabled = enabled(GL_SAMPLE_MASK);
                for (std::size_t index = 0;
                     index < snapshot.sample_mask_words.size();
                     ++index) {
                    GLint word = 0;
                    glGetIntegeri_v(
                        GL_SAMPLE_MASK_VALUE,
                        static_cast<GLuint>(index),
                        &word);
                    snapshot.sample_mask_words[index] =
                        static_cast<std::uint32_t>(word);
                }
                snapshot.sample_alpha_to_coverage_enabled = enabled(
                    GL_SAMPLE_ALPHA_TO_COVERAGE);
                snapshot.sample_alpha_to_one_enabled = enabled(
                    GL_SAMPLE_ALPHA_TO_ONE);
                snapshot.sample_coverage_enabled = enabled(
                    GL_SAMPLE_COVERAGE);
                glGetFloatv(
                    GL_SAMPLE_COVERAGE_VALUE,
                    &snapshot.sample_coverage_value);
                glGetBooleanv(
                    GL_SAMPLE_COVERAGE_INVERT,
                    &sample_coverage_inverted);
                snapshot.sample_shading_enabled = enabled(GL_SAMPLE_SHADING);

                snapshot.polygon_offset_fill_enabled = enabled(
                    GL_POLYGON_OFFSET_FILL);
                snapshot.polygon_offset_line_enabled = enabled(
                    GL_POLYGON_OFFSET_LINE);
                snapshot.polygon_offset_point_enabled = enabled(
                    GL_POLYGON_OFFSET_POINT);
                snapshot.depth_clamp_enabled = enabled(GL_DEPTH_CLAMP);
                snapshot.primitive_restart_enabled = enabled(
                    GL_PRIMITIVE_RESTART);
                snapshot.primitive_restart_fixed_index_enabled = enabled(
                    GL_PRIMITIVE_RESTART_FIXED_INDEX);
                snapshot.dither_enabled = enabled(GL_DITHER);
                snapshot.polygon_smooth_enabled = enabled(GL_POLYGON_SMOOTH);
                snapshot.stencil_test_enabled = enabled(GL_STENCIL_TEST);

                glGetIntegerv(GL_CLIP_ORIGIN, &clip_origin);
                glGetIntegerv(GL_CLIP_DEPTH_MODE, &clip_depth_mode);
                for (std::size_t index = 0;
                     index < snapshot.clip_distance_enabled.size();
                     ++index) {
                    snapshot.clip_distance_enabled[index] = enabled(
                        GL_CLIP_DISTANCE0 + static_cast<GLenum>(index));
                }
            });
        !queried) {
        return std::unexpected(std::move(queried.error()));
    }

    snapshot.draw_framebuffer = static_cast<std::uint32_t>(draw_framebuffer);
    snapshot.read_framebuffer = static_cast<std::uint32_t>(read_framebuffer);
    snapshot.program = static_cast<std::uint32_t>(program);
    snapshot.vertex_array = static_cast<std::uint32_t>(vertex_array);
    snapshot.depth_write_enabled = depth_write == GL_TRUE;
    snapshot.depth_compare = static_cast<std::uint32_t>(depth_compare);
    snapshot.cull_mode = static_cast<std::uint32_t>(cull_mode);
    snapshot.front_face = static_cast<std::uint32_t>(front_face);
    snapshot.polygon_mode = static_cast<std::uint32_t>(polygon_mode[0]);
    snapshot.sample_coverage_inverted =
        sample_coverage_inverted == GL_TRUE;
    snapshot.clip_origin = static_cast<std::uint32_t>(clip_origin);
    snapshot.clip_depth_mode = static_cast<std::uint32_t>(clip_depth_mode);

    return RenderStateScope(device.state_, std::move(snapshot));
}

std::expected<void, Diagnostic> RenderStateScope::restore()
{
    if (!active_) {
        return {};
    }
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "RenderStateScope::restore has no device state",
        });
    }
    if (auto current = state_->require_current("RenderStateScope::restore"); !current) {
        return current;
    }

    // Run every restoration command before inspecting the error flag. If one
    // saved object was deleted inside the scope, unrelated state still gets
    // the best possible restoration.
    auto restored = detail::checked_gl_call(
        "restore OpenGL render state",
        [&] {
            glBindFramebuffer(
                GL_DRAW_FRAMEBUFFER,
                snapshot_.draw_framebuffer);
            glBindFramebuffer(
                GL_READ_FRAMEBUFFER,
                snapshot_.read_framebuffer);
            for (std::size_t index = 0;
                 index < snapshot_.viewports.size();
                 ++index) {
                const auto gl_index = static_cast<GLuint>(index);
                const auto& viewport = snapshot_.viewports[index];
                glViewportIndexedfv(gl_index, viewport.viewport.data());
                set_indexed_capability(
                    GL_SCISSOR_TEST,
                    gl_index,
                    viewport.scissor_enabled);
                glScissorIndexedv(gl_index, viewport.scissor_box.data());
                glDepthRangeIndexed(
                    gl_index,
                    viewport.depth_range[0],
                    viewport.depth_range[1]);
            }
            glUseProgram(snapshot_.program);
            glBindVertexArray(snapshot_.vertex_array);

            set_capability(GL_DEPTH_TEST, snapshot_.depth_test_enabled);
            glDepthMask(snapshot_.depth_write_enabled ? GL_TRUE : GL_FALSE);
            glDepthFunc(snapshot_.depth_compare);

            for (const auto& draw_buffer : snapshot_.draw_buffers) {
                set_indexed_capability(
                    GL_BLEND,
                    draw_buffer.index,
                    draw_buffer.blend_enabled);
                glBlendFuncSeparatei(
                    draw_buffer.index,
                    draw_buffer.blend_factors[0],
                    draw_buffer.blend_factors[1],
                    draw_buffer.blend_factors[2],
                    draw_buffer.blend_factors[3]);
                glBlendEquationSeparatei(
                    draw_buffer.index,
                    draw_buffer.blend_equations[0],
                    draw_buffer.blend_equations[1]);
                glColorMaski(
                    draw_buffer.index,
                    draw_buffer.color_write[0] ? GL_TRUE : GL_FALSE,
                    draw_buffer.color_write[1] ? GL_TRUE : GL_FALSE,
                    draw_buffer.color_write[2] ? GL_TRUE : GL_FALSE,
                    draw_buffer.color_write[3] ? GL_TRUE : GL_FALSE);
            }

            set_capability(GL_CULL_FACE, snapshot_.cull_enabled);
            glCullFace(snapshot_.cull_mode);
            glFrontFace(snapshot_.front_face);

            set_capability(
                GL_RASTERIZER_DISCARD,
                snapshot_.rasterizer_discard_enabled);
            set_capability(
                GL_FRAMEBUFFER_SRGB,
                snapshot_.framebuffer_srgb_enabled);

            set_capability(
                GL_COLOR_LOGIC_OP,
                snapshot_.color_logic_op_enabled);
            glPolygonMode(GL_FRONT_AND_BACK, snapshot_.polygon_mode);

            for (std::size_t index = 0;
                 index < snapshot_.sample_mask_words.size();
                 ++index) {
                glSampleMaski(
                    static_cast<GLuint>(index),
                    snapshot_.sample_mask_words[index]);
            }
            set_capability(GL_SAMPLE_MASK, snapshot_.sample_mask_enabled);
            set_capability(
                GL_SAMPLE_ALPHA_TO_COVERAGE,
                snapshot_.sample_alpha_to_coverage_enabled);
            set_capability(
                GL_SAMPLE_ALPHA_TO_ONE,
                snapshot_.sample_alpha_to_one_enabled);
            glSampleCoverage(
                snapshot_.sample_coverage_value,
                snapshot_.sample_coverage_inverted ? GL_TRUE : GL_FALSE);
            set_capability(
                GL_SAMPLE_COVERAGE,
                snapshot_.sample_coverage_enabled);
            set_capability(
                GL_SAMPLE_SHADING,
                snapshot_.sample_shading_enabled);

            set_capability(
                GL_POLYGON_OFFSET_FILL,
                snapshot_.polygon_offset_fill_enabled);
            set_capability(
                GL_POLYGON_OFFSET_LINE,
                snapshot_.polygon_offset_line_enabled);
            set_capability(
                GL_POLYGON_OFFSET_POINT,
                snapshot_.polygon_offset_point_enabled);
            set_capability(GL_DEPTH_CLAMP, snapshot_.depth_clamp_enabled);
            set_capability(
                GL_PRIMITIVE_RESTART,
                snapshot_.primitive_restart_enabled);
            set_capability(
                GL_PRIMITIVE_RESTART_FIXED_INDEX,
                snapshot_.primitive_restart_fixed_index_enabled);
            set_capability(GL_DITHER, snapshot_.dither_enabled);
            set_capability(
                GL_POLYGON_SMOOTH,
                snapshot_.polygon_smooth_enabled);
            set_capability(GL_STENCIL_TEST, snapshot_.stencil_test_enabled);

            glClipControl(snapshot_.clip_origin, snapshot_.clip_depth_mode);
            for (std::size_t index = 0;
                 index < snapshot_.clip_distance_enabled.size();
                 ++index) {
                set_capability(
                    GL_CLIP_DISTANCE0 + static_cast<GLenum>(index),
                    snapshot_.clip_distance_enabled[index] != 0);
            }
        });

    // Native restoration may differ from the desired state of a command
    // stream used inside this scope. The next managed update must reassert it
    // even when the requested setting equals its cached desired value.
    state_->graphics_synchronized = false;
    // A scope can restore a native program different from the last managed
    // selection inside it. Keep handles live, but require explicit reselection
    // instead of letting run() skip a bind based on a now-stale program cache.
    state_->command_program = nullptr;
    state_->command_view_ready = false;
    active_ = false;
    state_.reset();
    if (!restored) {
        return std::unexpected(std::move(restored.error()));
    }
    return {};
}

void RenderStateScope::restore_noexcept() noexcept
{
    if (!active_) {
        return;
    }
    if (state_ && state_->is_current()) {
        auto state = state_;
        auto restored = restore();
        if (!restored) {
            state->record_lifecycle_failure(
                "RenderStateScope failed to restore OpenGL state: "
                + restored.error().message);
        }
        return;
    }
    if (state_) {
        state_->record_lifecycle_failure(
            "RenderStateScope destroyed without its owning context current; OpenGL state was not restored");
    }
    active_ = false;
    state_.reset();
}

} // namespace vng::opengl
