#pragma once

#include <vng/content/vmesh.hpp>
#include <vng/spatial/triangle_bvh.hpp>
#include <memory>
#include <optional>
#include <span>

namespace vng::editor {
// Shared by direct mesh import and embedded scene/draft readers.
[[nodiscard]] content::vmesh::Limits mesh_limits();
// Editable format document. Position edits preserve storage; topology edits
// validate a candidate before committing. Unrelated fields/metadata survive.
class EditableMesh {
public:
    [[nodiscard]] static content::Result<EditableMesh> create(content::vmesh::Document);
    [[nodiscard]] static content::Result<EditableMesh> load(const std::filesystem::path&);
    [[nodiscard]] const content::vmesh::Document& document() const noexcept { return document_; }
    [[nodiscard]] std::size_t size() const noexcept { return document_.vertex_count; }
    [[nodiscard]] Vec3 position(u32 vertex) const;
    // Vertex-average pivot (zero for an empty mesh). Cached independently of
    // placement/camera transforms; geometry edits invalidate the shared snapshot.
    [[nodiscard]] Vec3 center() const;
    // Geometry-owned immutable snapshot, shared by mesh copies/instances.
    // The first query prepares it; vertex edits invalidate it, and the next query
    // rebuilds once for the whole edit batch. Like edits, cache access is owner-thread only.
    [[nodiscard]] const spatial::TriangleBvh& picking_index() const;
    [[nodiscard]] content::Result<void> set_position(u32 vertex, Vec3);
    [[nodiscard]] content::Result<void> translate(std::span<const u32> vertices, Vec3 delta);
    [[nodiscard]] std::vector<u32> coincident(u32 vertex, f32 tolerance = 0.00001F) const;
    // Includes triangle boundaries and explicit loose edges, canonicalized and unique.
    [[nodiscard]] std::vector<gfx::Edge> edges() const;
    // Two vertices create an edge; 3+ describe an ordered polygon boundary.
    // Polygons are triangulated without introducing vertex records.
    [[nodiscard]] content::Result<void> fill(std::span<const u32> ordered_vertices);
    // One midpoint per selected edge, shared by every incident triangle.
    // Floating attributes are averaged; integral attributes copy the lower-ID endpoint.
    [[nodiscard]] content::Result<std::vector<u32>> subdivide(std::span<const gfx::Edge>);
    // First two vertices are anchors and remain bit-for-bit unchanged.
    [[nodiscard]] content::Result<std::vector<u32>> align_to_line(std::span<const u32> ordered_vertices);

private:
    explicit EditableMesh(content::vmesh::Document d, std::size_t field)
        : document_(std::move(d)), position_field_(field) {}
    content::vmesh::Document document_;
    std::size_t position_field_{};
    // Copies share even the not-yet-built cache. A geometry edit detaches it;
    // building an index for one unchanged snapshot benefits all its copies.
    struct GeometryCache {
        std::shared_ptr<const spatial::TriangleBvh> index;
        std::optional<Vec3> center;
    };
    mutable std::shared_ptr<GeometryCache> geometry_{std::make_shared<GeometryCache>()};
};
} // namespace vng::editor
