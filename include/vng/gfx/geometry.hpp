#pragma once

#include <vng/core/types.hpp>
#include <vng/gfx/semantic.hpp>

namespace vng::gfx {

// Convenience semantics for engine-provided geometry renderers. Applications
// may keep their own semantic types and map them once at a renderer boundary.
struct Position : Semantic<Vec3> {};
struct Normal : Semantic<Vec3> {};
struct Color : Semantic<Vec4> {};
template<u32 Set = 0> struct TexCoord : Semantic<Vec2> {};

template<u32 Set = 0> struct JointIndices : Semantic<UVec4> {};
template<u32 Set = 0> struct JointWeights : Semantic<Vec4> {};

} // namespace vng::gfx
