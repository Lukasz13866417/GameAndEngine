#pragma once
#include <vng/core/types.hpp>

namespace example::earth {
// Physical reference shared by Earth-space authoring and kilometre-space shots.
// Diameters are clear vertical bores at unit authoring multipliers.
enum class TunnelSizeClass : vng::u32 { local, regional, trunk };
inline constexpr vng::f32 reference_earth_radius_km=6371.F;
inline constexpr vng::f32 tunnel_inner_half_height=.00594F;
inline constexpr vng::f32 tunnel_width_reduction=.6F;
[[nodiscard]] constexpr vng::f32 tunnel_diameter_km(TunnelSizeClass size) {
    switch(size) {
    case TunnelSizeClass::local:return 6.F;
    case TunnelSizeClass::regional:return 24.F;
    case TunnelSizeClass::trunk:return 2*tunnel_inner_half_height*reference_earth_radius_km*tunnel_width_reduction;
    }
    return 0;
}
[[nodiscard]] constexpr vng::f32 tunnel_width_coefficient(TunnelSizeClass size) {
    return tunnel_diameter_km(size)/(2*tunnel_inner_half_height*reference_earth_radius_km);
}
inline constexpr vng::f32 cinematic_tunnel_radius_km=tunnel_diameter_km(TunnelSizeClass::local)*.5F;
}
