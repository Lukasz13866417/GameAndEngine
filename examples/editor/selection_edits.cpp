#include "selection_edits.hpp"
#include "project.hpp"

#include <array>

namespace editor_example {
namespace {
using namespace vng;
constexpr std::array<char, 8> magic{'V', 'N', 'G', 'S', 'E', 'L', 0, 1};
constexpr std::size_t payload_bytes = 32;
constexpr u64 max_revision = (u64{1} << 53) - 1;

auto invalid(std::string message) {
    content::Diagnostic diagnostic;
    diagnostic.code = content::ErrorCode::invalid_document;
    diagnostic.message = std::move(message);
    return std::unexpected(std::move(diagnostic));
}
content::Result<void> validate(const SelectionEdit& edit) {
    if (!edit.base_revision || edit.revision <= edit.base_revision || edit.revision > max_revision)
        return invalid("Selection edit requires 1 <= base < target <= 2^53-1");
    if (edit.selected_vertex >= 65536)
        return invalid("Selection vertex exceeds the mesh vertex limit");
    return {};
}
content::Result<void> validate(const State& state, const SelectionEdit& edit) {
    if (auto valid = validate(edit); !valid) return valid;
    if (edit.selected_object && !find_instance(state, edit.selected_object))
        return invalid("Selection edit references a missing scene instance");
    // A scene selection never changes the explicitly inspected blueprint.
    // Validate the proposed selection without copying or mutating State.
    const auto* mesh = state.viewport.mode == ViewMode::mesh
        ? mesh_geometry(state, state.viewport.inspected_mesh)
        : state.viewport.mode == ViewMode::scene ? instance_mesh(state, edit.selected_object) : nullptr;
    if (state.viewport.mode == ViewMode::mesh && !mesh)
        return invalid("Selection edit requires an existing inspected mesh blueprint");
    if ((!mesh && edit.selected_vertex != 0) || (mesh && edit.selected_vertex >= mesh->size()))
        return invalid("Selection vertex is outside the selected/viewed mesh");
    return {};
}
void integer(std::string& bytes, u64 value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<char>((value >> (i * 8U)) & 255U));
}
u64 integer(std::string_view bytes, std::size_t& offset, unsigned width) {
    u64 result{};
    for (unsigned i = 0; i < width; ++i)
        result |= u64(static_cast<unsigned char>(bytes[offset++])) << (i * 8U);
    return result;
}
} // namespace

content::Result<std::string> encode_selection_edit(const SelectionEdit& edit) {
    if (auto valid = validate(edit); !valid) return std::unexpected(valid.error());
    std::string bytes;
    bytes.reserve(payload_bytes);
    bytes.append(magic.data(), magic.size());
    integer(bytes, edit.base_revision, 8);
    integer(bytes, edit.revision, 8);
    integer(bytes, edit.selected_object, 4);
    integer(bytes, edit.selected_vertex, 4);
    return bytes;
}
content::Result<SelectionEdit> decode_selection_edit(std::string_view bytes) {
    if (bytes.size() != payload_bytes)
        return invalid("Selection edit payload must contain exactly 32 bytes");
    if (bytes.substr(0, magic.size()) != std::string_view(magic.data(), magic.size()))
        return invalid("Selection edit payload has an unknown magic or version");
    std::size_t offset = magic.size();
    SelectionEdit edit;
    edit.base_revision = integer(bytes, offset, 8);
    edit.revision = integer(bytes, offset, 8);
    edit.selected_object = static_cast<u32>(integer(bytes, offset, 4));
    edit.selected_vertex = static_cast<u32>(integer(bytes, offset, 4));
    if (auto valid = validate(edit); !valid) return std::unexpected(valid.error());
    return edit;
}
content::Result<SelectionEdit> selection_edit(u64 base_revision, const State& state) {
    SelectionEdit edit{base_revision, state.document.revision, state.viewport.selected_object, state.viewport.selected_vertex};
    if (auto valid = validate(state, edit); !valid) return std::unexpected(valid.error());
    return edit;
}
content::Result<void> apply_selection_edit(State& state, const SelectionEdit& edit) {
    if (auto valid = validate(state, edit); !valid) return valid;
    if (state.document.revision != edit.base_revision)
        return invalid("Stale selection edit base revision; resynchronize before applying");
    state.viewport.selected_object = edit.selected_object;
    state.viewport.selected_vertex = edit.selected_vertex;
    state.document.revision = edit.revision;
    return {};
}
} // namespace editor_example
