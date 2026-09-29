#pragma once
#include <vng/shader/shader.hpp>

// Haze of the `render/lighting = tunnel` and `tunnel_departure*` materials and
// of the departure's mouth veil: one curve, so the veil matches the walls it
// covers. An art-directed approximation, not volumetric scattering.
namespace editor_example::tunnel_haze {
// Kilometres at which the optical depth reaches 1.
inline constexpr vng::f32 reach_km=18.F;
// Optical depth (d/18)^4 from a squared camera distance in kilometres: nearby
// panels stay readable, and beyond 18 km the bore whitens faster than the
// original (d/18)^2. Keeping (d/18)^2 past 18 km looked the same in every
// departure frame (at most 4 of 255 levels), as the veil is white there.
template<class Squared> auto depth(Squared distance2) {
    const auto x=distance2/(reach_km*reach_km);
    return x*x;
}
}
