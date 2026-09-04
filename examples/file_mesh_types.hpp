#pragma once

#include <vng/gfx/gfx.hpp>

namespace file_mesh_example {

// The file schema and every render path share these domain-level semantic
// types. Shader contracts remain private to whichever path defines them.
struct Position : vng::gfx::Semantic<vng::Vec3> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};

// Both shader-owning paths expose the same optional analysis observation.
// It does not become a vertex field or ordinary fragment output.
struct SurfaceColor : vng::gfx::Semantic<vng::Vec4> {};

using Vertex = vng::gfx::Record<
    Position,
    vng::gfx::as<Color, vng::gfx::unorm8x4>>;

} // namespace file_mesh_example
