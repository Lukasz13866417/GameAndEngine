#pragma once

#include <expected>
#include <memory>

#include <vng/core/types.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/render/graphics_state.hpp>

namespace vng::opengl {

// An OpenGL extension to the portable setting vocabulary.
enum class PolygonMode { fill, line, point };

// Value snapshot of the managed draw state, independent of shaders and target
// encoding. Expert raw GL must be reconciled with Commands::restore() first.
struct GraphicsStateSnapshot final {
    render::DepthState depth{};
    render::CullMode cull{render::CullMode::none};
    render::FrontFace front_face{render::FrontFace::counter_clockwise};
    render::BlendMode blend{render::BlendMode::disabled};
    PolygonMode polygon{PolygonMode::fill};
    friend constexpr bool operator==(
        const GraphicsStateSnapshot&, const GraphicsStateSnapshot&) = default;
};
class Commands;

namespace detail {
struct ContextState;

// Holds context lifetime and generation tokens, never a pointer to a C++
// Commands/Frame object. Copies share live state; expired scopes reject use.
class GraphicsStateAccess final {
public:
    [[nodiscard]] std::expected<void, Diagnostic> set(render::DepthTest);
    [[nodiscard]] std::expected<void, Diagnostic> set(render::DepthWrite);
    [[nodiscard]] std::expected<void, Diagnostic> set(render::DepthCompare);
    [[nodiscard]] std::expected<void, Diagnostic> set(render::DepthState);
    [[nodiscard]] std::expected<void, Diagnostic> set(render::CullMode);
    [[nodiscard]] std::expected<void, Diagnostic> set(render::FrontFace);
    [[nodiscard]] std::expected<void, Diagnostic> set(render::BlendMode);
    [[nodiscard]] std::expected<void, Diagnostic> set(PolygonMode);
    [[nodiscard]] std::expected<void, Diagnostic> set(GraphicsStateSnapshot);
    [[nodiscard]] std::expected<GraphicsStateSnapshot, Diagnostic> snapshot() const;

private:
    GraphicsStateAccess(std::shared_ptr<ContextState> state, u64 frame_generation) noexcept
        : state_(std::move(state)), frame_generation_(frame_generation) {}

    [[nodiscard]] std::expected<void, Diagnostic> validate() const;
    [[nodiscard]] std::expected<void, Diagnostic> synchronize();
    std::shared_ptr<ContextState> state_;
    u64 frame_generation_{};
    friend class ::vng::opengl::Commands;
};
} // namespace detail

using GraphicsState = render::GraphicsState<detail::GraphicsStateAccess>;

} // namespace vng::opengl
