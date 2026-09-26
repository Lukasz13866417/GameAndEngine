#pragma once
#include <vng/content/vmesh.hpp>
#include <optional>
#include "earth_tunnel_sizes.hpp"

namespace example::earth {
// Complete blueprint budget (terrain, clouds and infrastructure). Indices are
// u32; the extra capacity accommodates real tunnel linings on dense Earths.
inline constexpr std::size_t max_earth_vertices=131072;
// Optional, blueprint-owned authoring layer. The result remains ordinary mesh
// geometry, with no runtime particles, per-city objects, or extra draw calls.
struct InfrastructureSettings {
    bool night_lights{}, skyways{}, launch_hubs{};
    vng::f32 light_strength{1}, city_density{1}, skyway_height{1};
    vng::f32 structure_size{1}, hub_height{1};
    bool large_structures{};
    bool processors{};
    vng::f32 addon_scale{1}, settlement_scale{1}, tunnel_scale{1}, launch_pad_scale{1};
    vng::f32 terminal_scale{1}, elevator_scale{1}, joiner_scale{1}, processor_scale{1};
    friend bool operator==(const InfrastructureSettings&,const InfrastructureSettings&)=default;
};
[[nodiscard]] bool valid(InfrastructureSettings);
[[nodiscard]] vng::content::Result<InfrastructureSettings>
infrastructure_settings(const vng::content::vmesh::Document&);
// Replaces only vertices owned by earth/infrastructure. Terrain, cloud edits,
// custom fields, and the whole-blueprint authoring frame are retained.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document>
rebuild_infrastructure(const vng::content::vmesh::Document&,InfrastructureSettings);
// Explicitly regenerate one recipe (and tunnels attached to it), retaining all
// other parts' geometry, including hand edits and custom vertex attributes.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document>
rebuild_infrastructure_part(const vng::content::vmesh::Document&,vng::u32);

// Local instances of the infrastructure recipes. Identities survive edits,
// rebuilds, serialization and removal of neighboring parts. They are not scene
// instances and own no renderer or GPU resource.
enum class InfrastructureKind : vng::u32 { settlement=1, skyway=2, hub=3, terminal=4, elevator=5, joiner=6, processor=7 };
// Shared by geometry, connection frames, and editor handles. Authored part
// dimensions remain relative values; baseline edits never rewrite them.
[[nodiscard]] vng::f32 addon_scale(InfrastructureSettings,InfrastructureKind);
[[nodiscard]] std::string_view infrastructure_name(InfrastructureKind);
[[nodiscard]] bool infrastructure_enabled(InfrastructureKind, const InfrastructureSettings&);
// A socket belongs to a structure, never to another tunnel. Zero means ground.
// These stable identities are authored data, not geometry indices or pointers.
struct TunnelSocketRef {
    vng::u32 part{}, socket{};
    explicit operator bool() const {return part!=0;}
    friend bool operator==(const TunnelSocketRef&,const TunnelSocketRef&)=default;
};
inline constexpr vng::f32 max_infrastructure_altitude=10;
// Includes the full envelope of raised/scaled structure sockets, so removing a
// structure or detaching an end never fails merely because its port was high.
inline constexpr vng::f32 max_tunnel_endpoint_altitude=512;
struct InfrastructurePart {
    vng::u32 id{};
    InfrastructureKind kind{InfrastructureKind::hub};
    std::string name;
    vng::Vec2 location{}, end{9,0}; // A/B for skyways; surface anchor otherwise.
    vng::f32 heading{}, size{1}, height{1};
    bool visible{true};
    vng::u32 seed{1};
    // Owned by a skyway, not separate parts: moving its anchors also moves
    // these fittings. Old v1 recipes load with both ends unadorned.
    bool terminal_a{}, terminal_b{};
    TunnelSocketRef socket_a{}, socket_b{};
    vng::f32 altitude_a{}, altitude_b{}; // Free-end height above the usual endpoint shell.
    vng::f32 altitude{}, scale{1}; // Standalone height and overall recipe dimensions.
    bool scaffold{true};
    vng::f32 scaffold_spacing{}; // Tunnel arc distance, Earth radii; zero keeps automatic end supports.
    // Empty optional = automatic spacing. An engaged (possibly empty) list is
    // authored placement along the route, in [0,1]. Order is handle identity;
    // do not sort when a support crosses a neighbor. End edits carry them along.
    std::optional<std::vector<vng::f32>> scaffold_positions{};
    TunnelSizeClass tunnel_class{TunnelSizeClass::trunk}; // Legacy routes shrink by 40%.
    // Canonical Earth-space interior control points. Disengaged = automatic
    // arch; engaged = Bezier, with the existing A/B anchors as its endpoints.
    std::optional<std::vector<vng::Vec3>> bezier_controls{};
    vng::u32 curve_segments{48};
    // Additional upward inclination of a flared mouth, in degrees. The route
    // tangent still supplies its underlying heading and grade.
    vng::f32 terminal_incline{30};
    friend bool operator==(const InfrastructurePart&,const InfrastructurePart&)=default;
};
inline constexpr std::string_view infrastructure_part_field="earth/infrastructure-part";
[[nodiscard]] bool has_infrastructure_parts(const vng::content::vmesh::Document&);
[[nodiscard]] bool valid(const InfrastructurePart&);
[[nodiscard]] vng::Vec2 infrastructure_center(const InfrastructurePart&);
[[nodiscard]] vng::content::Result<std::vector<InfrastructurePart>> infrastructure_parts(const vng::content::vmesh::Document&);
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> add_infrastructure(
    const vng::content::vmesh::Document&, InfrastructureKind, vng::Vec2 location={0,20});
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> remove_infrastructure(const vng::content::vmesh::Document&,vng::u32);
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> edit_infrastructure(const vng::content::vmesh::Document&,const InfrastructurePart&);
// Rigid moves preserve hand edits and every custom field. Endpoint/property
// edits regenerate only the selected recipe; unrelated parts stay untouched.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> move_infrastructure(const vng::content::vmesh::Document&,vng::u32,vng::Vec2);
// Position of the whole-part handle in canonical Earth coordinates. Preserves
// altitude during a surface move; changes it when the radial handle is used.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> place_infrastructure(const vng::content::vmesh::Document&,vng::u32,vng::Vec3);
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> rotate_infrastructure(const vng::content::vmesh::Document&,vng::u32,vng::f32);
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> move_infrastructure_endpoint(const vng::content::vmesh::Document&,vng::u32,bool end,vng::Vec2);
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> place_infrastructure_endpoint(const vng::content::vmesh::Document&,vng::u32,bool end,vng::Vec3);
// An occupied socket rejects a second tunnel. Detaching (an empty reference)
// preserves the former socket's position, including altitude.
[[nodiscard]] vng::content::Result<vng::content::vmesh::Document> connect_infrastructure_endpoint(
    const vng::content::vmesh::Document&,vng::u32 tunnel,bool end,TunnelSocketRef);
}
