#pragma once
#include <vng/gfx/record.hpp>

namespace editor_example::mesh_shading {
struct Eye : vng::gfx::Semantic<vng::Vec3> {};
struct Light : vng::gfx::Semantic<vng::Vec4> {};
struct Brightness : vng::gfx::Semantic<vng::f32> {};
using Lighting = vng::gfx::Record<Eye,Light,Brightness>;

// One visible scene instance produces one backend-neutral ticket. Geometry
// belongs to the blueprint renderer, not to tickets or individual instances.
struct MeshDraw {
    vng::Mat4 transform{vng::Mat4::identity()};
    Lighting lighting;
    bool wireframe{};
};
} // namespace editor_example::mesh_shading
