#include <vng/opengl/graphics_state.hpp>
#include <vng/opengl/device.hpp>

#include "context_state.hpp"
#include "gl_error.hpp"

#include <array>

namespace vng::opengl::detail {
namespace {

template<class Enum, std::size_t N>
std::expected<GLenum, Diagnostic> translate(
    Enum value, const std::array<GLenum, N>& values)
{
    const auto index = static_cast<std::size_t>(value);
    if (index >= N) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "GraphicsState::set received an invalid setting value",
        });
    }
    return values[index];
}

constexpr std::array<GLenum, 8> comparisons{
    GL_NEVER, GL_LESS, GL_LEQUAL, GL_EQUAL,
    GL_GEQUAL, GL_GREATER, GL_NOTEQUAL, GL_ALWAYS};
constexpr std::array<GLenum, 3> culling{GL_BACK, GL_FRONT, GL_BACK};
constexpr std::array<GLenum, 2> winding{GL_CW, GL_CCW};
constexpr std::array<GLenum, 3> polygons{GL_FILL, GL_LINE, GL_POINT};

void apply_blend(render::BlendMode mode)
{
    if (mode == render::BlendMode::disabled) glDisablei(GL_BLEND, 0);
    else glEnablei(GL_BLEND, 0);
    glBlendEquationSeparatei(0, GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparatei(0,
        mode == render::BlendMode::straight_alpha ? GL_SRC_ALPHA : GL_ONE,
        mode == render::BlendMode::additive ? GL_ONE :
            (mode == render::BlendMode::disabled ? GL_ZERO : GL_ONE_MINUS_SRC_ALPHA),
        mode == render::BlendMode::additive ? GL_ZERO : GL_ONE,
        mode == render::BlendMode::additive ? GL_ONE :
            (mode == render::BlendMode::disabled ? GL_ZERO : GL_ONE_MINUS_SRC_ALPHA));
}

void apply_depth(render::DepthState depth)
{
    if (depth.test) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    glDepthMask(depth.write ? GL_TRUE : GL_FALSE);
    glDepthFunc(comparisons[static_cast<std::size_t>(depth.compare)]);
}

void apply_cull(render::CullMode mode)
{
    if (mode == render::CullMode::none) glDisable(GL_CULL_FACE);
    else glEnable(GL_CULL_FACE);
    glCullFace(culling[static_cast<std::size_t>(mode)]);
}

} // namespace

std::expected<void, Diagnostic> GraphicsStateAccess::validate() const
{
    if (!state_) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_context_access,
            .message = "GraphicsState::set used an empty context",
        });
    }
    if (auto current = state_->require_current("GraphicsState::set"); !current) {
        return current;
    }
    if (!state_->frame_is_active(frame_generation_)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_argument,
            .message = "GraphicsState::set used a stale or ended frame command stream",
        });
    }
    if (state_->graphics_generation != frame_generation_) {
        state_->graphics_depth = {};
        state_->graphics_cull = render::CullMode::none;
        state_->graphics_front_face = render::FrontFace::counter_clockwise;
        state_->graphics_polygon = PolygonMode::fill;
        state_->graphics_blend = render::BlendMode::disabled;
        state_->graphics_synchronized = false;
        state_->graphics_generation = frame_generation_;
    }
    return {};
}

std::expected<void, Diagnostic> GraphicsStateAccess::synchronize()
{
    if (auto valid = validate(); !valid) return valid;
    if (state_->graphics_synchronized) return {};

    // A fresh frame establishes deterministic defaults lazily. Thereafter
    // only changed setting groups touch GL. restore() also uses this path
    // after expert code disturbed ambient state.
    Device device{state_};
    if (auto result = device.set_standard_raster_state(); !result) return result;
    if (auto result = device.reset_color_outputs(); !result)
        return result;
    auto result = checked_gl_call("synchronize graphics state", [&] {
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_RASTERIZER_DISCARD);
        apply_depth(state_->graphics_depth);
        apply_cull(state_->graphics_cull);
        apply_blend(state_->graphics_blend);
        glFrontFace(winding[static_cast<std::size_t>(state_->graphics_front_face)]);
        glPolygonMode(GL_FRONT_AND_BACK,
                      polygons[static_cast<std::size_t>(state_->graphics_polygon)]);
    });
    state_->graphics_synchronized = result.has_value();
    return result;
}

std::expected<void, Diagnostic> GraphicsStateAccess::set(render::DepthTest value)
{
    if (auto valid = validate(); !valid) return valid;
    auto depth = state_->graphics_depth;
    depth.test = value.enabled;
    return set(depth);
}

std::expected<void, Diagnostic> GraphicsStateAccess::set(render::DepthWrite value)
{
    if (auto valid = validate(); !valid) return valid;
    auto depth = state_->graphics_depth;
    depth.write = value.enabled;
    return set(depth);
}

std::expected<void, Diagnostic> GraphicsStateAccess::set(render::DepthCompare value)
{
    if (auto valid = validate(); !valid) return valid;
    auto depth = state_->graphics_depth;
    depth.compare = value;
    return set(depth);
}

std::expected<void, Diagnostic> GraphicsStateAccess::set(render::DepthState value)
{
    if (auto valid = validate(); !valid) return valid;
    auto native = translate(value.compare, comparisons);
    if (!native) return std::unexpected(std::move(native.error()));
    if (auto ready = synchronize(); !ready) return ready;
    if (state_->graphics_depth == value) return {};
    auto result = checked_gl_call("set depth state", [&] { apply_depth(value); });
    if (result) state_->graphics_depth = value;
    else state_->graphics_synchronized = false;
    return result;
}

std::expected<void, Diagnostic> GraphicsStateAccess::set(render::CullMode value)
{
    if (auto valid = validate(); !valid) return valid;
    auto native = translate(value, culling);
    if (!native) return std::unexpected(std::move(native.error()));
    if (auto ready = synchronize(); !ready) return ready;
    if (state_->graphics_cull == value) return {};
    auto result = checked_gl_call("set culling", [&] { apply_cull(value); });
    if (result) state_->graphics_cull = value;
    else state_->graphics_synchronized = false;
    return result;
}

std::expected<void, Diagnostic> GraphicsStateAccess::set(render::FrontFace value)
{
    if (auto valid = validate(); !valid) return valid;
    auto native = translate(value, winding);
    if (!native) return std::unexpected(std::move(native.error()));
    if (auto ready = synchronize(); !ready) return ready;
    if (state_->graphics_front_face == value) return {};
    auto result = checked_gl_call("set front-face winding", [&] { glFrontFace(*native); });
    if (result) state_->graphics_front_face = value;
    else state_->graphics_synchronized = false;
    return result;
}

std::expected<void, Diagnostic> GraphicsStateAccess::set(PolygonMode value)
{
    if (auto valid = validate(); !valid) return valid;
    auto native = translate(value, polygons);
    if (!native) return std::unexpected(std::move(native.error()));
    if (auto ready = synchronize(); !ready) return ready;
    if (state_->graphics_polygon == value) return {};
    auto result = checked_gl_call("set polygon mode", [&] {
        glPolygonMode(GL_FRONT_AND_BACK, *native);
    });
    if (result) state_->graphics_polygon = value;
    else state_->graphics_synchronized = false;
    return result;
}

std::expected<void, Diagnostic> GraphicsStateAccess::set(render::BlendMode value)
{
    if (auto valid = validate(); !valid) return valid;
    if (value != render::BlendMode::disabled && value != render::BlendMode::straight_alpha
        && value != render::BlendMode::premultiplied_alpha && value != render::BlendMode::additive) {
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "GraphicsState::set received an invalid blend mode"});
    }
    if (auto ready = synchronize(); !ready) return ready;
    if (state_->graphics_blend == value) return {};
    auto result = checked_gl_call("set alpha blending", [&] { apply_blend(value); });
    if (result) state_->graphics_blend = value;
    else state_->graphics_synchronized = false;
    return result;
}

std::expected<GraphicsStateSnapshot, Diagnostic> GraphicsStateAccess::snapshot() const
{
    if (auto valid = validate(); !valid) return std::unexpected(std::move(valid.error()));
    return GraphicsStateSnapshot{
        .depth = state_->graphics_depth,
        .cull = state_->graphics_cull,
        .front_face = state_->graphics_front_face,
        .blend = state_->graphics_blend,
        .polygon = state_->graphics_polygon,
    };
}

std::expected<void, Diagnostic> GraphicsStateAccess::set(GraphicsStateSnapshot value)
{
    if (auto valid = validate(); !valid) return valid;
    // Validate the complete value before modifying any state group.
    if (auto check = translate(value.depth.compare, comparisons); !check)
        return std::unexpected(std::move(check.error()));
    if (auto check = translate(value.cull, culling); !check)
        return std::unexpected(std::move(check.error()));
    if (auto check = translate(value.front_face, winding); !check)
        return std::unexpected(std::move(check.error()));
    if (auto check = translate(value.polygon, polygons); !check)
        return std::unexpected(std::move(check.error()));
    if (value.blend != render::BlendMode::disabled
        && value.blend != render::BlendMode::straight_alpha
        && value.blend != render::BlendMode::premultiplied_alpha
        && value.blend != render::BlendMode::additive) {
        return std::unexpected(Diagnostic{.code = ErrorCode::invalid_argument,
            .message = "GraphicsState::set received an invalid blend mode"});
    }
    if (auto result = set(value.depth); !result) return result;
    if (auto result = set(value.cull); !result) return result;
    if (auto result = set(value.front_face); !result) return result;
    if (auto result = set(value.blend); !result) return result;
    return set(value.polygon);
}

} // namespace vng::opengl::detail
