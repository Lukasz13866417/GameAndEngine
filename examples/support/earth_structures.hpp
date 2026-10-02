#pragma once
#include "earth_skyway.hpp"
#include "earth_tunnel_sizes.hpp"
#include <vng/gfx/mesh.hpp>
#include <array>
#include <span>

namespace example::earth::detail {
// Small CPU geometry recipes. No document IDs, editor, or GPU ownership here;
// the infrastructure builder assigns the resulting geometry to its parent part.
struct StructureVertex {
    vng::Vec3 position, normal, color;
    vng::f32 emission{};
};
struct StructureMesh {
    std::vector<StructureVertex> vertices;
    std::vector<vng::gfx::TriangleFace> faces;
};
using TerminalPath = std::array<SkywaySample, 9>;
// One tunnel construction for orbital and interior views. Samples control route
// tessellation only; the regular-octagonal shell, lining and lighting stay identical.
struct TunnelSection { SkywaySample frame; vng::f32 size; };
inline constexpr vng::f32 tunnel_inner_height = tunnel_inner_half_height;
[[nodiscard]] StructureMesh tunnel_shell(std::span<const TunnelSection>, vng::f32 light);
[[nodiscard]] StructureMesh tunnel_collar(const SkywaySample&, vng::f32 size);
[[nodiscard]] StructureMesh dispersal_terminal(const TerminalPath&, vng::f32 size, vng::f32 light);
// Canonical coordinates: Y radial up, Z forward; the host supplies placement.
[[nodiscard]] TerminalPath freestanding_terminal_path(vng::f32 size, vng::f32 height);
[[nodiscard]] StructureMesh orbital_elevator(vng::f32 size, vng::f32 height, vng::f32 light);
[[nodiscard]] StructureMesh atmospheric_processor(vng::f32 size,vng::f32 height,vng::f32 light);
// Local outward frames: inlet, left outlet, right outlet. Used for both the
// visible open ports and their attachment contracts.
[[nodiscard]] std::array<SkywaySample,3> joiner_sockets(vng::f32 size,vng::f32 height);
[[nodiscard]] StructureMesh tunnel_joiner(vng::f32 size,vng::f32 height,vng::f32 light);
[[nodiscard]] StructureMesh tunnel_scaffold(const SkywaySample&,vng::f32 size);
}
