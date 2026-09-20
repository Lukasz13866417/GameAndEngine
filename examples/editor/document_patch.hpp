#pragma once
#include "document_changes.hpp"
#include "edits.hpp"
#include "project.hpp"

namespace editor_example {
struct PropertyPatch {
    vng::timeline::Target target;
    vng::timeline::Value base;
    std::optional<vng::timeline::Track> track; // null removes only this property's track
    friend bool operator==(const PropertyPatch&, const PropertyPatch&) = default;
};
struct RegionPointEdit {
    vng::u32 index{};
    vng::Vec3 position{};
    friend bool operator==(const RegionPointEdit&, const RegionPointEdit&) = default;
};
struct RegionPatch {
    vng::u32 id{};
    std::optional<Region> replacement{}; // topology/name/note: only this instance
    vng::u32 point_count{}; // sparse patches require unchanged indexing
    std::vector<RegionPointEdit> points{};
    friend bool operator==(const RegionPatch&, const RegionPatch&) = default;
};
struct DocumentPatch {
    vng::u64 base_revision{}, revision{};
    std::vector<VertexEdit> vertices{};
    std::vector<PropertyPatch> properties{};
    std::optional<vng::f32> duration{};
    std::map<vng::f32, std::optional<std::string>> markers{}; // null removes the name/empty marker
    std::optional<WorldBounds> world_bounds{};
    std::vector<RegionPatch> regions{};
    struct MeshDraft {
        vng::u32 blueprint{};
        bool present{};
        vng::editor::MeshPatch values;
        friend bool operator==(const MeshDraft&,const MeshDraft&)=default;
    };
    std::vector<MeshDraft> meshes{};
    std::map<BlueprintId, std::optional<MeshPlacement>> mesh_placements{};
    friend bool operator==(const DocumentPatch&, const DocumentPatch&) = default;
};
[[nodiscard]] DocumentChanges changes_of(const DocumentPatch&);
// Timeline authoring already has these small before/after tables; never scan meshes.
[[nodiscard]] DocumentChanges animation_changes(const Document&, const Document&);
[[nodiscard]] vng::content::Result<DocumentPatch> capture_patch(vng::u64 base, const State&, const DocumentChanges&);
[[nodiscard]] vng::content::Result<void> apply_patch(State&, const DocumentPatch&);
[[nodiscard]] vng::content::Result<std::string> encode_patch(const DocumentPatch&);
[[nodiscard]] vng::content::Result<DocumentPatch> decode_patch(std::string_view);
} // namespace editor_example
