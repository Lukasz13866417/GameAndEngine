#pragma once

#include <vng/editor/mesh.hpp>
#include <map>
#include <set>

namespace vng::editor {
// Change identity, not a cached document. Merges retain exact attribute IDs.
struct MeshChanges {
    bool whole{}; // topology or field schema changed
    std::map<std::string, std::set<u32>, std::less<>> fields{};
    std::set<std::string, std::less<>> metadata{};
    [[nodiscard]] bool empty() const { return !whole && fields.empty() && metadata.empty(); }
    void merge(const MeshChanges& other);
    friend bool operator==(const MeshChanges&, const MeshChanges&) = default;
};

struct AttributePatch {
    content::vmesh::VertexField field;
    std::vector<u32> vertices; // sorted, unique; values follow this order
    friend bool operator==(const AttributePatch&, const AttributePatch&) = default;
};

struct MeshPatch {
    std::optional<content::vmesh::Document> replacement{};
    std::size_t vertex_count{}; // guard against applying sparse edits to new indexing
    std::vector<AttributePatch> fields{};
    std::map<std::string, std::optional<std::string>, std::less<>> metadata{};
    friend bool operator==(const MeshPatch&, const MeshPatch&) = default;
};

// Compare/capture validated documents. A producer that already knows its change
// scope can supply MeshChanges directly and skip comparison entirely.
[[nodiscard]] MeshChanges mesh_changes(const content::vmesh::Document& before,
                                       const content::vmesh::Document& after);
[[nodiscard]] MeshChanges changes_of(const MeshPatch&);
[[nodiscard]] content::Result<MeshPatch> capture_mesh_patch(const content::vmesh::Document&,
                                                          const MeshChanges&);
// Produces a validated candidate without mutating the source on failure.
[[nodiscard]] content::Result<EditableMesh> apply_mesh_patch(const EditableMesh&, const MeshPatch&);
[[nodiscard]] content::Result<void> validate_mesh_patch(const MeshPatch&);

// Bounded binary transport. File formats remain unchanged.
[[nodiscard]] content::Result<std::string> encode_mesh_patch(const MeshPatch&);
[[nodiscard]] content::Result<MeshPatch> decode_mesh_patch(std::string_view);
} // namespace vng::editor
