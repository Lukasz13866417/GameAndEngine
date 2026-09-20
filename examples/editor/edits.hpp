#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vng/content/diagnostic.hpp>
#include <vng/core/types.hpp>

namespace vng::editor {
class EditableMesh;
}

namespace editor_example {
struct State;
struct VertexPosition {
    vng::u32 index{};
    vng::Vec3 position{};
    friend bool operator==(const VertexPosition&, const VertexPosition&) = default;
};
struct VertexEdit {
    vng::u64 base_revision{}, revision{};
    std::vector<VertexPosition> vertices;
    vng::u32 blueprint{1};
    friend bool operator==(const VertexEdit&, const VertexEdit&) = default;
};

// Absolute positions, not movement deltas. A coalesced patch must contain every
// vertex changed since base_revision, even if intermediate frames were skipped.
// The enclosing preview connection owns worker-generation identity. Never
// change base_revision on an old packet unless its entries still cover the
// complete change from the newly acknowledged mesh.
//
// Wire v1: 8-byte magic/version, LE u64 base + target, LE u32 count, then
// count * {LE u32 index, three IEEE754 binary32 components}; 28 + 16*N bytes.
// Wire v2 for imported blueprints adds LE u32 blueprint after count (32-byte
// header). Default mesh retains v1 encoding for existing tools/fixtures.
// Strictly bounded to the editor slice's 65536 vertices / +/-1e6 positions.
[[nodiscard]] vng::content::Result<std::string> encode_edit(const VertexEdit&);
[[nodiscard]] vng::content::Result<VertexEdit> decode_edit(std::string_view);

// Validates the complete packet before modifying anything. Records only the
// touched before-positions for rollback and writes the target revision last;
// topology, other attributes, settings and untouched storage are unchanged.
[[nodiscard]] vng::content::Result<void> apply_edit(State&, const VertexEdit&);

// Diff helpers reject changes that a position-only patch cannot represent.
[[nodiscard]] vng::content::Result<VertexEdit> vertex_edit(const vng::editor::EditableMesh& before,
                                                           const vng::editor::EditableMesh& after,
                                                           vng::u64 base_revision,
                                                           vng::u64 revision);
[[nodiscard]] vng::content::Result<VertexEdit> vertex_edit(const State& before, const State& after);

// Fast path for a known dirty set: O(touched vertices), no whole-mesh scan or
// copy. Caller guarantees this set contains all position changes since base.
[[nodiscard]] vng::content::Result<VertexEdit>
vertex_edit(vng::u64 base_revision, const State& current, std::span<const vng::u32> touched);
} // namespace editor_example
