#pragma once

#include <vng/content/diagnostic.hpp>
#include <vng/core/types.hpp>
#include <string>
#include <string_view>

namespace editor_example {
struct State;

// Editor view state only: this does not author scene content or change assets.
struct SelectionEdit {
    vng::u64 base_revision{}, revision{};
    vng::u32 selected_object{}, selected_vertex{};
    friend bool operator==(const SelectionEdit&, const SelectionEdit&) = default;
};

// Fixed 32-byte LE v1 payload: magic/version, two u64 revisions, two u32 IDs.
// The containing control message adds the "selection\n" prefix.
[[nodiscard]] vng::content::Result<std::string> encode_selection_edit(const SelectionEdit&);
[[nodiscard]] vng::content::Result<SelectionEdit> decode_selection_edit(std::string_view);
[[nodiscard]] vng::content::Result<SelectionEdit> selection_edit(vng::u64 base_revision, const State&);
// Validate identity, vertex bounds and exact base before writing any field.
// Never copies or traverses mesh geometry; zero object means no selection.
[[nodiscard]] vng::content::Result<void> apply_selection_edit(State&, const SelectionEdit&);
} // namespace editor_example
