#pragma once
#include <vng/core/types.hpp>
#include <string>
#include <optional>

namespace editor_example {
// Blueprint-local surface constraint, with no UI, GPU or document ownership.
struct SurfaceMove {
    vng::Vec3 center{}, position{};
    vng::f32 radius{1};
    std::string label;
    // Constraint coordinates remain local even after whole-blueprint edits.
    vng::Mat4 frame{vng::Mat4::identity()};
    bool can_rotate{};
    friend bool operator==(const SurfaceMove&, const SurfaceMove&) = default;
};
struct SurfacePartAction {
    bool began{}, changed{}, finished{}, cancelled{};
    vng::Vec3 position{};
    std::optional<vng::f32> rotation_degrees; // Relative surface-normal turn, from gesture start.
};
}
