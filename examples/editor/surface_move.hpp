#pragma once
#include <vng/core/types.hpp>
#include <string>
#include <optional>
#include <memory>
#include "move_path.hpp"

namespace editor_example {
struct PartScale {
    vng::f32 value{1},maximum{20};
    friend bool operator==(const PartScale&,const PartScale&)=default;
};
// Blueprint-local surface constraint, with no UI, GPU or document ownership.
struct SurfaceMove {
    vng::Vec3 center{}, position{};
    vng::f32 radius{1};
    std::string label;
    // Constraint coordinates remain local even after whole-blueprint edits.
    vng::Mat4 frame{vng::Mat4::identity()};
    bool can_rotate{};
    std::optional<vng::Vec2> radial_range{}; // Optional min/max radius: adds an up/down handle.
    // An optional blueprint-declared route restricts movement to that curve
    // instead of the full spherical surface. Shared by its pickable handles.
    std::shared_ptr<const MovePath> path{};
    std::optional<PartScale> scale{}; // Exclusive uniform-scale handle instead of surface movement.
    friend bool operator==(const SurfaceMove&, const SurfaceMove&) = default;
};
struct SurfacePartAction {
    bool began{}, changed{}, finished{}, cancelled{};
    vng::Vec3 position{};
    std::optional<vng::f32> rotation_degrees; // Relative surface-normal turn, from gesture start.
    std::optional<std::size_t> selected_handle; // Selection only; not an authored edit.
    std::optional<vng::f32> scale_value; // Absolute per-part multiplier, not the blueprint baseline.
};
}
