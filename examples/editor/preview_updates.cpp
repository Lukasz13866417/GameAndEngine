#include "preview_updates.hpp"
#include "project.hpp"
#include "document_patch.hpp"

namespace editor_example {
namespace {
bool single_vertex_batch_only(const DocumentPatch& patch) {
    // A compact packet must cover the entire patch before we acknowledge its
    // revision. Never discard another category merely because vertices exist.
    return patch.mesh_placements.empty() && patch.meshes.empty() && patch.vertices.size() == 1 && patch.properties.empty() &&
        !patch.duration && !patch.world_bounds && patch.regions.empty() && patch.markers.empty();
}
}
void PreviewUpdates::add(vng::u64 generation) { peers_.try_emplace(generation); }
void PreviewUpdates::remove(vng::u64 generation) { peers_.erase(generation); }
void PreviewUpdates::reset(vng::u64 generation) { peers_[generation] = Peer{}; }
void PreviewUpdates::reject(vng::u64 generation) {
    auto& peer = peers_[generation];
    peer.sent = 0; peer.changes = {.full = true}; peer.rejected = true;
    peer.playback = peer.selection = false;
}
void PreviewUpdates::changed() { changed(DocumentChanges{.full = true}); }
void PreviewUpdates::changed(const DocumentChanges& changes) {
    for (auto& [id, peer] : peers_) {
        (void)id;
        peer.rejected = false;
        peer.known = true;
        peer.changes.merge(changes);
    }
}
void PreviewUpdates::changed(std::span<const vng::u32> vertices, vng::u32 blueprint) {
    if (vertices.empty()) return;
    DocumentChanges changes;
    changes.vertices[blueprint].insert(vertices.begin(), vertices.end());
    changed(changes);
}
// Legacy revision-bearing selection/playback packets remain readable for old
// clients. The application uses the independently sequenced view lane.
void PreviewUpdates::playback_changed() {
    for (auto& [id, peer] : peers_) { (void)id; peer.rejected = false; peer.playback = true; }
}
void PreviewUpdates::selection_changed() {
    for (auto& [id, peer] : peers_) { (void)id; peer.rejected = false; peer.selection = true; }
}
void PreviewUpdates::position_changed(vng::u32 object) { transform_changed(object, "position"); }
void PreviewUpdates::rotation_changed(vng::u32 object) { transform_changed(object, "rotation"); }
void PreviewUpdates::scale_changed(vng::u32 object) { transform_changed(object, "scale"); }
void PreviewUpdates::transform_changed(vng::u32 object, std::string_view property) {
    DocumentChanges changes;
    if (!object) changes.full = true;
    else changes.properties.insert({object, std::string(property)});
    changed(changes);
}
void PreviewUpdates::acknowledge(vng::u64 generation, vng::u64 revision) {
    auto found = peers_.find(generation);
    if (found == peers_.end() || !found->second.sent || found->second.sent != revision) return;
    found->second.base = revision; found->second.sent = 0;
}
void PreviewUpdates::accepted(vng::u64 generation, vng::u64 revision) {
    peers_[generation] = Peer{.base = revision, .changes = {}};
}
bool PreviewUpdates::ready(vng::u64 generation, vng::u64 revision) const {
    const auto found = peers_.find(generation);
    if (found == peers_.end()) return false;
    const auto& p = found->second;
    return p.base == revision && !p.sent && !p.known && p.changes.empty() && !p.playback && !p.selection;
}
vng::content::Result<std::optional<std::string>> PreviewUpdates::next(vng::u64 generation, const State& state) {
    auto found = peers_.find(generation);
    if (found == peers_.end()) return std::optional<std::string>{};
    auto& peer = found->second;
    if (state.document.revision < peer.base) {
        vng::content::Diagnostic error;
        error.message = "Cannot send an editor revision older than its acknowledged baseline";
        return std::unexpected(std::move(error));
    }
    if (peer.rejected || peer.sent || ready(generation, state.document.revision)) return std::optional<std::string>{};
    auto changes = peer.changes;
    const bool legacy_mixed = (peer.selection || peer.playback) &&
        (!changes.empty() || (peer.selection && peer.playback));
    std::string packet;
    if (changes.full || !peer.base || legacy_mixed ||
        (changes.empty() && !peer.known && !peer.playback && !peer.selection)) {
        auto bytes = encode(state);
        if (!bytes) return std::unexpected(bytes.error());
        packet = "snapshot\n" + std::move(*bytes);
    } else if (peer.selection) {
        auto edit = selection_edit(peer.base, state);
        if (!edit) return std::unexpected(edit.error());
        auto bytes = encode_selection_edit(*edit);
        if (!bytes) return std::unexpected(bytes.error());
        packet = "selection\n" + std::move(*bytes);
    } else if (peer.playback) {
        auto edit = playback_edit(peer.base, state);
        if (!edit) return std::unexpected(edit.error());
        auto bytes = encode_playback_edit(*edit);
        if (!bytes) return std::unexpected(bytes.error());
        packet = "playback\n" + std::move(*bytes);
    } else {
        // Keep the tiny single-property encodings. A mixed dirty set is a
        // versioned patch, never a reason to serialize scene geometry.
        if (changes.mesh_placements.empty() && changes.meshes.empty() && changes.vertices.empty() && !changes.duration && !changes.world_bounds && changes.regions.empty() && changes.markers.empty() && changes.properties.size() == 1 &&
            changes.properties.begin()->object <= UINT32_MAX) {
            const auto& target = *changes.properties.begin();
            const auto object = static_cast<vng::u32>(target.object);
            const auto encode_one = [&](auto capture, auto encode_value, std::string_view prefix) -> vng::content::Result<void> {
                auto edit = capture(peer.base, state, object);
                if (!edit) return std::unexpected(edit.error());
                auto bytes = encode_value(*edit);
                if (!bytes) return std::unexpected(bytes.error());
                packet = std::string(prefix) + std::move(*bytes); return {};
            };
            vng::content::Result<void> result;
            if (target.property == "position") result = encode_one(position_edit, encode_position_edit, "position\n");
            else if (target.property == "rotation") result = encode_one(rotation_edit, encode_rotation_edit, "rotation\n");
            else if (target.property == "scale") result = encode_one(scale_edit, encode_scale_edit, "scale\n");
            if (!result) return std::unexpected(result.error());
        }
        if (packet.empty()) {
            auto patch = capture_patch(peer.base, state, changes);
            if (!patch) return std::unexpected(patch.error());
            if (single_vertex_batch_only(*patch)) {
                auto bytes = encode_edit(patch->vertices.front());
                if (!bytes) return std::unexpected(bytes.error());
                packet = "vertices\n" + std::move(*bytes);
            } else {
                auto bytes = encode_patch(*patch);
                if (!bytes) return std::unexpected(bytes.error());
                packet = "patch\n" + std::move(*bytes);
            }
        }
    }
    peer.sent = state.document.revision;
    peer.changes = {};
    peer.known = false;
    peer.playback = peer.selection = false;
    return std::optional{std::move(packet)};
}
} // namespace editor_example
