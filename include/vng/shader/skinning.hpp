#pragma once

#include <vng/shader/stage.hpp>

namespace vng::dsl {

// An ordinary inline DSL helper, not a new IR operation or an OpenGL API.
// Four influences are unrolled at shader-build time; indices remain dynamic.
// Offset/stride let a renderer choose its own matrix-buffer organization.
// Every index must be in range, including padding whose weight is zero.
// The caller also guarantees that joint * stride + offset does not overflow.
template<u32 Binding, class Stage>
[[nodiscard]] Float4x4 blend_matrices(
    Stage& stage, Expr<UVec4> joints, Float4 weights,
    u32 offset = 0, u32 stride = 1)
{
    const auto read = [&](UInt joint) {
        // Palette layout is explicit shader-build configuration. Bone indices
        // remain expressions; these CPU layout values are deliberately baked.
        return stage.template matrix_buffer<Binding>(
            joint * stage.constant(stride) + stage.constant(offset));
    };
    const auto x = read(joints.x()) * weights.x();
    const auto y = read(joints.y()) * weights.y();
    const auto z = read(joints.z()) * weights.z();
    const auto w = read(joints.w()) * weights.w();
    return ((x + y) + z) + w;
}

} // namespace vng::dsl
