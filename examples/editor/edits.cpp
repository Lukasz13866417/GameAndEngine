#include "edits.hpp"
#include "project.hpp"

#include <array>
#include <bit>
#include <bitset>
#include <cmath>
#include <limits>

namespace editor_example {
namespace {
using namespace vng;
constexpr std::size_t max_vertices = 65536;
constexpr std::size_t header_size = 28, entry_size = 16;
constexpr u64 max_revision = (u64{1} << 53) - 1;
constexpr std::array<char, 8> magic{'V', 'N', 'G', 'V', 'T', 'X', 0, 1};
static_assert(sizeof(f32) == sizeof(u32) && std::numeric_limits<f32>::is_iec559);

auto invalid(std::string message) {
    content::Diagnostic diagnostic;
    diagnostic.code = content::ErrorCode::invalid_document;
    diagnostic.message = std::move(message);
    return std::unexpected(std::move(diagnostic));
}
bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::abs(value.x) <= 1000000 && std::abs(value.y) <= 1000000 &&
           std::abs(value.z) <= 1000000;
}
content::Result<void> revisions(u64 base, u64 target) {
    if (!base || target <= base || target > max_revision)
        return invalid("Vertex edit requires 1 <= base < target <= 2^53-1");
    return {};
}
content::Result<void> validate(const VertexEdit& edit) {
    if (auto result = revisions(edit.base_revision, edit.revision); !result)
        return result;
    if (!edit.blueprint || edit.blueprint == 2)
        return invalid("Vertex edit requires a mesh blueprint identity");
    if (edit.vertices.size() > max_vertices)
        return invalid("Vertex edit exceeds the 65536-vertex limit");
    std::bitset<max_vertices> seen;
    for (const auto& vertex : edit.vertices) {
        if (vertex.index >= max_vertices)
            return invalid("Vertex edit index exceeds the editor limit");
        if (seen.test(vertex.index))
            return invalid("Vertex edit contains a duplicate index");
        seen.set(vertex.index);
        if (!finite(vertex.position))
            return invalid("Vertex edit position must be finite and within +/-1e6");
    }
    return {};
}
void integer(std::string& bytes, u64 value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<char>((value >> (i * 8)) & 255));
}
u64 integer(std::string_view bytes, std::size_t& offset, unsigned width) {
    // The fixed header and total payload length are checked before any reads.
    u64 result{};
    for (unsigned i = 0; i < width; ++i)
        result |= static_cast<u64>(static_cast<unsigned char>(bytes[offset++])) << (i * 8);
    return result;
}
bool same_mesh_contract(const editor::EditableMesh& before, const editor::EditableMesh& after) {
    const auto& a = before.document();
    const auto& b = after.document();
    if (a.vertex_count != b.vertex_count || a.metadata != b.metadata || a.faces != b.faces ||
        a.edges != b.edges || a.vertex_fields.size() != b.vertex_fields.size())
        return false;
    for (std::size_t i = 0; i < a.vertex_fields.size(); ++i) {
        const auto& x = a.vertex_fields[i];
        const auto& y = b.vertex_fields[i];
        if (x.name != y.name || x.type != y.type || (x.name != "position" && x.values != y.values))
            return false;
    }
    return true;
}
} // namespace

vng::content::Result<std::string> encode_edit(const VertexEdit& edit) {
    if (auto result = validate(edit); !result)
        return std::unexpected(result.error());
    std::string bytes;
    bytes.reserve(header_size + 4 + edit.vertices.size() * entry_size);
    bytes.append(magic.data(), magic.size());
    if (edit.blueprint != 1) bytes[7] = 2;
    integer(bytes, edit.base_revision, 8);
    integer(bytes, edit.revision, 8);
    integer(bytes, edit.vertices.size(), 4);
    if (edit.blueprint != 1) integer(bytes, edit.blueprint, 4);
    for (const auto& vertex : edit.vertices) {
        integer(bytes, vertex.index, 4);
        integer(bytes, std::bit_cast<vng::u32>(vertex.position.x), 4);
        integer(bytes, std::bit_cast<vng::u32>(vertex.position.y), 4);
        integer(bytes, std::bit_cast<vng::u32>(vertex.position.z), 4);
    }
    return bytes;
}
vng::content::Result<VertexEdit> decode_edit(std::string_view bytes) {
    using namespace vng;
    if (bytes.size() < header_size || bytes.size() > header_size + 4 + max_vertices * entry_size)
        return invalid("Vertex edit payload has an invalid length");
    if (bytes.substr(0, magic.size() - 1) != std::string_view(magic.data(), magic.size() - 1) ||
        (bytes[7] != 1 && bytes[7] != 2))
        return invalid("Vertex edit payload has an unknown magic or version");
    const std::size_t decoded_header_size = header_size + (bytes[7] == 2 ? 4 : 0);
    if (bytes.size() < decoded_header_size) return invalid("Vertex edit payload has an invalid length");
    std::size_t offset = magic.size();
    VertexEdit edit;
    edit.base_revision = integer(bytes, offset, 8);
    edit.revision = integer(bytes, offset, 8);
    const auto count = integer(bytes, offset, 4);
    if (bytes[7] == 2) edit.blueprint = static_cast<u32>(integer(bytes, offset, 4));
    if (count > max_vertices || bytes.size() != decoded_header_size + count * entry_size)
        return invalid("Vertex edit payload length does not match its bounded count");
    if (auto result = revisions(edit.base_revision, edit.revision); !result)
        return std::unexpected(result.error());
    edit.vertices.reserve(static_cast<std::size_t>(count));
    for (u64 i = 0; i < count; ++i) {
        VertexPosition vertex;
        vertex.index = static_cast<u32>(integer(bytes, offset, 4));
        vertex.position.x = std::bit_cast<f32>(static_cast<u32>(integer(bytes, offset, 4)));
        vertex.position.y = std::bit_cast<f32>(static_cast<u32>(integer(bytes, offset, 4)));
        vertex.position.z = std::bit_cast<f32>(static_cast<u32>(integer(bytes, offset, 4)));
        edit.vertices.push_back(vertex);
    }
    if (auto result = validate(edit); !result)
        return std::unexpected(result.error());
    return edit;
}
vng::content::Result<void> apply_edit(State& state, const VertexEdit& edit) {
    if (auto result = validate(edit); !result)
        return result;
    if (state.document.revision != edit.base_revision)
        return invalid("Stale vertex edit base revision; resynchronize before applying");
    auto* geometry = mesh_edit_geometry(state, static_cast<BlueprintId>(edit.blueprint));
    if (!geometry) return invalid("Vertex edit references a missing mesh blueprint");
    for (const auto& vertex : edit.vertices)
        if (vertex.index >= geometry->size())
            return invalid("Vertex edit index is outside this mesh");

    // Allocation and all data validation finish before the first write. On the
    // validated path, EditableMesh::set_position only assigns three floats.
    std::vector<VertexPosition> before;
    before.reserve(edit.vertices.size());
    for (const auto& vertex : edit.vertices)
        before.push_back({vertex.index, geometry->position(vertex.index)});
    struct Transaction {
        vng::editor::EditableMesh& mesh;
        const std::vector<VertexPosition>& before;
        std::size_t written{};
        bool committed{};
        ~Transaction() {
            if (!committed)
                for (std::size_t i = 0; i < written; ++i)
                    (void)mesh.set_position(before[i].index, before[i].position);
        }
    } transaction{*geometry, before};
    for (const auto& vertex : edit.vertices) {
        if (auto result = geometry->set_position(vertex.index, vertex.position); !result)
            return result;
        ++transaction.written;
    }
    state.document.revision = edit.revision;
    transaction.committed = true;
    return {};
}
vng::content::Result<VertexEdit> vertex_edit(const vng::editor::EditableMesh& before,
                                             const vng::editor::EditableMesh& after,
                                             vng::u64 base_revision, vng::u64 revision) {
    if (auto result = revisions(base_revision, revision); !result)
        return std::unexpected(result.error());
    if (!same_mesh_contract(before, after))
        return invalid(
            "Position-only edit cannot represent changed topology, metadata, or attributes");
    VertexEdit edit{base_revision, revision, {}};
    for (vng::u32 i = 0; i < before.size(); ++i)
        if (before.position(i) != after.position(i))
            edit.vertices.push_back({i, after.position(i)});
    return edit;
}
vng::content::Result<VertexEdit> vertex_edit(const State& before, const State& after) {
    if (before.document.instances != after.document.instances || before.document.mesh_blueprint != after.document.mesh_blueprint ||
        before.document.sun_blueprint != after.document.sun_blueprint || before.document.effect_assets != after.document.effect_assets || before.document.next_instance_id != after.document.next_instance_id ||
        before.document.next_blueprint_id != after.document.next_blueprint_id || before.document.mesh_assets.size() != after.document.mesh_assets.size() ||
        before.viewport.mode != after.viewport.mode || before.viewport.inspected_mesh != after.viewport.inspected_mesh ||
        before.viewport.selected_object != after.viewport.selected_object ||
        before.viewport.selected_vertex != after.viewport.selected_vertex || before.viewport.weld != after.viewport.weld ||
        before.viewport.paused != after.viewport.paused || before.viewport.time != after.viewport.time ||
        before.viewport.editor_camera != after.viewport.editor_camera || before.document.timeline != after.document.timeline ||
        before.document.timeline_duration != after.document.timeline_duration || before.document.keyframe_names != after.document.keyframe_names)
        return invalid("Position-only edit cannot represent changed scene settings or selection");
    const auto target = mesh_target(after);
    if (!target) return invalid("Position-only edit requires an explicit mesh target");
    const auto blueprint = target->blueprint;
    for (std::size_t i = 0; i < before.document.mesh_assets.size(); ++i) {
        const auto& a = before.document.mesh_assets[i];
        const auto& b = after.document.mesh_assets[i];
        if (a.id != b.id || a.name != b.name || a.settings != b.settings ||
            (a.id != blueprint && a.geometry.document() != b.geometry.document()))
            return invalid("Position-only edit cannot represent changes to other mesh blueprints");
    }
    if (blueprint != BlueprintId::mesh && before.document.mesh.document() != after.document.mesh.document())
        return invalid("Position-only edit cannot represent changes to other mesh blueprints");
    if(before.document.mesh_drafts.size()!=after.document.mesh_drafts.size())
        return invalid("Draft creation/publication requires a snapshot");
    for(const auto& [id,draft]:before.document.mesh_drafts) {
        const auto next=after.document.mesh_drafts.find(id);
        if(next==after.document.mesh_drafts.end() || (id!=blueprint && draft.document()!=next->second.document()))
            return invalid("Position-only edit cannot change another mesh draft");
    }
    const auto* a = mesh_edit_geometry(before, blueprint);
    const auto* b = mesh_edit_geometry(after, blueprint);
    if (!a || !b) return invalid("Position-only edit requires an existing mesh blueprint");
    auto result = vertex_edit(*a, *b, before.document.revision, after.document.revision);
    if (result) result->blueprint = static_cast<u32>(blueprint);
    return result;
}
vng::content::Result<VertexEdit> vertex_edit(vng::u64 base_revision, const State& current,
                                             std::span<const vng::u32> touched) {
    if (auto result = revisions(base_revision, current.document.revision); !result)
        return std::unexpected(result.error());
    if (touched.size() > max_vertices)
        return invalid("Vertex edit touched set exceeds the editor limit");
    const auto target = mesh_target(current);
    const auto* geometry = editable_mesh(current);
    if (!target || !geometry)
        return invalid("Vertex edit requires an explicit mesh target");
    VertexEdit edit{base_revision, current.document.revision, {}, static_cast<u32>(target->blueprint)};
    edit.vertices.reserve(touched.size());
    std::bitset<max_vertices> seen;
    for (const auto index : touched) {
        if (index >= geometry->size() || index >= max_vertices)
            return invalid("Vertex edit touched index is outside this mesh");
        if (seen.test(index))
            continue;
        seen.set(index);
        edit.vertices.push_back({index, geometry->position(index)});
    }
    return edit;
}
} // namespace editor_example
