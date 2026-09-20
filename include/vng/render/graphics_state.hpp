#pragma once

#include <utility>
#include <type_traits>

namespace vng::render {

struct DepthTest final { bool enabled; };
struct DepthWrite final { bool enabled; };

enum class DepthCompare {
    never, less, less_equal, equal, greater_equal, greater, not_equal, always,
};

// Optional grouped update; individual DepthTest/DepthWrite/DepthCompare
// settings preserve all other choices. No persistent preset is required.
struct DepthState final {
    bool test{false};
    bool write{false};
    DepthCompare compare{DepthCompare::less};
    friend constexpr bool operator==(const DepthState&, const DepthState&) = default;
};

enum class CullMode { none, front, back };
enum class FrontFace { clockwise, counter_clockwise };
// Color attachment 0. Shader output for premultiplied_alpha must already
// contain RGB multiplied by opacity; alpha is composited independently.
// Additive sums emitted RGB light and preserves destination alpha.
enum class BlendMode { disabled, straight_alpha, premultiplied_alpha, additive };

template<class State, class Setting>
concept SupportsGraphicsSetting = requires(State& state, Setting setting) {
    state.set(setting);
};

// A statically dispatched facade. Backends supply a small frame-scoped access
// object with set overloads for portable settings and backend extensions.
// There is no backend registry, virtual dispatch, or per-setting allocation.
template<class BackendState>
class GraphicsState final {
public:
    explicit GraphicsState(BackendState backend)
        noexcept(std::is_nothrow_move_constructible_v<BackendState>)
        : backend_(std::move(backend)) {}

    template<class Setting>
        requires SupportsGraphicsSetting<BackendState, Setting>
    [[nodiscard]] auto set(Setting setting)
        noexcept(noexcept(backend_.set(setting)))
    {
        return backend_.set(setting);
    }

    // Backend snapshots include supported extensions without widening the
    // portable setting vocabulary. No retained preset belongs to a program.
    [[nodiscard]] auto snapshot() const
        requires requires(const BackendState& state) { state.snapshot(); }
    {
        return backend_.snapshot();
    }

private:
    BackendState backend_;
};

} // namespace vng::render
