#pragma once
#include "../editor/project.hpp"
#include "../support/earth_tunnel_sizes.hpp"

namespace example::tunnel {
// Scene units are kilometres. The camera travels only a small portion of the
// route; curvature, not a painted exit disk, blocks the far portal.
inline constexpr vng::f32 radius = earth::cinematic_tunnel_radius_km;
inline constexpr vng::f32 route_length = 1000.F;
inline constexpr vng::f32 bend_radius = 6371.F;
inline constexpr vng::f32 duration = 36.F;
[[nodiscard]] vng::Vec3 center(vng::f32 distance);
// Reusable geometry for the interior study and the departure cinematic. The
// exit frame has its mouth at zero, incoming tube along +Z and radial up +Y.
[[nodiscard]] vng::Vec3 exit_center(vng::f32 remaining);
[[nodiscard]] vng::content::vmesh::Document shell_mesh(bool exit_frame = false);
[[nodiscard]] vng::content::vmesh::Document collar_mesh();
[[nodiscard]] vng::content::Result<editor_example::State> author_scene(const std::filesystem::path& assets);
}
