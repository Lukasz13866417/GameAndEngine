#pragma once
#include <vng/core/types.hpp>
#include <vng/render/graphics_state.hpp>

namespace vng::render {
// Attachment encoding, not a change to logical DepthCompare/clear semantics.
// Reversed depth is useful with floating-point depth attachments at large scales.
enum class DepthMapping { standard, reversed };

[[nodiscard]] constexpr f32 encode_depth(f32 depth, DepthMapping mapping) noexcept
{
    return mapping == DepthMapping::reversed ? 1.F - depth : depth;
}

[[nodiscard]] constexpr DepthCompare encode_compare(
    DepthCompare compare, DepthMapping mapping) noexcept
{
    if (mapping == DepthMapping::reversed) {
        switch (compare) {
        case DepthCompare::less: return DepthCompare::greater;
        case DepthCompare::less_equal: return DepthCompare::greater_equal;
        case DepthCompare::greater: return DepthCompare::less;
        case DepthCompare::greater_equal: return DepthCompare::less_equal;
        default: break;
        }
    }
    return compare;
}
} // namespace vng::render
