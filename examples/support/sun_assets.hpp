#pragma once

#include <vng/gfx/geometry.hpp>
#include <vng/gfx/image.hpp>
#include <vng/gfx/mesh.hpp>

namespace example::sun {

struct SurfaceUV : vng::gfx::Semantic<vng::Vec2> {};
using SunVertex = vng::gfx::Record<vng::gfx::Position, SurfaceUV>;
using SunMesh = vng::gfx::Mesh<SunVertex>;

// Unit sphere, counter-clockwise from outside. U is longitude / (2*pi),
// V is colatitude / pi; the +Z direction is (0.25, 0.5). Seam positions
// are bit-identical, while U distinguishes 0 from 1. Polar caps contain
// one non-degenerate triangle per sector.
[[nodiscard]] SunMesh make_surface(
    vng::u32 longitude = 384, vng::u32 latitude = 192);

// Procedural *linear data*, not an sRGB photograph:
// R: tiny bright granules separated by thin dark intergranular lanes.
// G: mixed-scale short irregular tufts/finer strands, without long contour patterns.
// B: softened dark sunspot/filament mask, anchored to the same magnetic regions.
// A: arcade-footpoint emission envelopes plus three compact authored hot knots.
// All four fields are functions of a continuous 3D direction, so no UV
// seam or polar singularity is baked in. Sample with repeat U / clamp V.
[[nodiscard]] vng::gfx::ImageData make_surface_map(
    vng::u32 width = 2048, vng::u32 height = 1024);

// Normal stores the radial direction, intentionally not the tube's normal:
// it lets a shader gently displace each strand away from the solar surface.
// UV.x follows each arch from footpoint to footpoint; UV.y identifies the
// strand and provides a stable animation phase. TubeOffset is position minus
// the strand centerline, so a shader can widen a soft surrounding plasma sheath
// with position + TubeOffset * (scale - 1), without changing the arch itself.
struct TubeOffset : vng::gfx::Semantic<vng::Vec3> {};
using ProminenceVertex = vng::gfx::Record<
    vng::gfx::Position, vng::gfx::Normal, SurfaceUV, TubeOffset>;
using ProminenceMesh = vng::gfx::Mesh<ProminenceVertex>;

[[nodiscard]] ProminenceMesh make_prominences(vng::u32 seed = 0x501A2026U);

} // namespace example::sun
