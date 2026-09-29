#pragma once
#include <vng/shader/shader.hpp>

// Haze of the `render/lighting = tunnel` and `tunnel_departure*` materials and
// of the departure's mouth veil: one curve, so the veil matches the walls it
// covers. An art-directed approximation, not volumetric scattering.
namespace editor_example::tunnel_haze {
// Kilometres at which the optical depth reaches 1.
inline constexpr vng::f32 reach_km=18.F;
// false: (d/18)^4 at every distance. Nearby panels stay readable, and beyond
//        18 km the bore whitens faster than the original (d/18)^2 did.
// true:  (d/18)^4 up to 18 km, then the original (d/18)^2: the same gentle
//        near field with the original, softer far-field falloff.
inline constexpr bool original_far_field=false;
// Optical depth from a squared camera distance in kilometres.
template<class Squared> auto depth(Squared distance2) {
    const auto x=distance2/(reach_km*reach_km);
    if constexpr(original_far_field) return x*vng::dsl::min(x,1.F);
    else return x*x;
}
}
