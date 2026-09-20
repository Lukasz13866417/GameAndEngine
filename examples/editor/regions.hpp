#pragma once
#include "scene_coordinates.hpp"
#include <vng/content/diagnostic.hpp>
#include <vng/editor/scene_cage.hpp>
#include <vng/content/document.hpp>
#include <ostream>
#include <span>

namespace editor_example {
enum class RegionShape { box, tetrahedron, octahedron, prism };
inline constexpr vng::u32 region_center = UINT32_MAX;
inline constexpr std::size_t max_regions = 128, max_region_points = 1024;
inline constexpr std::size_t max_region_faces = 4096, max_region_corners = 16384;
using RegionFace = std::vector<vng::u32>;
using RegionEdge = std::array<vng::u32,2>;
enum class RegionAction { add_vertex, fill, subdivide, align, erase };
struct RegionGeometry {
    std::vector<vng::Vec3> points; // instance-local vertices; never scene entities
    std::vector<RegionFace> faces;
    std::vector<RegionEdge> loose_edges;
    [[nodiscard]] std::vector<RegionEdge> edges() const;
    [[nodiscard]] vng::Vec3 center() const;
    friend bool operator==(const RegionGeometry&, const RegionGeometry&) = default;
};
struct RegionSettings {
    RegionGeometry boundary;
    std::string note{"TODO: add something here"};
    bool visible{true};
    bool show_walls{}; // Editor-only opaque grid, per instance; not blueprint geometry.
    friend bool operator==(const RegionSettings&, const RegionSettings&) = default;
};
// Value-only cage/edit/legacy-file snapshot. SceneInstance owns identity,
// transform and RegionSettings; this is not another persistent object registry.
struct Region : RegionGeometry {
    vng::u32 id{};
    std::string name{"Region"}, note{"TODO: add something here"};
    bool show_walls{};
    [[nodiscard]] vng::editor::SceneCage scene_cage() const;
    friend bool operator==(const Region&, const Region&) = default;
};
struct Regions {
    vng::u32 next_id{1};
    std::vector<Region> items;
    friend bool operator==(const Regions&, const Regions&) = default;
};
[[nodiscard]] Region make_region(RegionShape, vng::Vec3 center, vng::f32 radius);
// Used only to migrate the old point-only format, never during vertex editing.
[[nodiscard]] vng::content::Result<void> migrate_region_hull(Region&);
[[nodiscard]] std::vector<vng::u32> region_vertices(const Region&, vng::editor::CageElement,
    std::span<const vng::u32> elements);
// Validates a candidate before assignment. Returned IDs are vertices to select.
[[nodiscard]] vng::content::Result<std::vector<vng::u32>> edit_region_geometry(
    Region&, RegionAction, vng::editor::CageElement, std::span<const vng::u32> elements);
[[nodiscard]] vng::content::Result<void> validate(const Region&);
[[nodiscard]] vng::content::Result<void> validate(const Regions&);
[[nodiscard]] const Region* find_region(const Regions&, vng::u32);
// Shared by native scene persistence and bounded document patches.
void write_regions(std::ostream&, const Regions&);
[[nodiscard]] Regions read_regions(vng::content::Reader);
void write_region_geometry(std::ostream&, const RegionGeometry&);
[[nodiscard]] RegionGeometry read_region_geometry(vng::content::Reader);
} // namespace editor_example
