#pragma once
#include "edits.hpp"
#include "playback.hpp"
#include "position_edits.hpp"
#include "rotation_edits.hpp"
#include "scale_edits.hpp"
#include "selection_edits.hpp"
#include "document_changes.hpp"
#include <map>
#include <optional>
#include <set>

namespace editor_example {
[[nodiscard]] constexpr bool accepts_completed_revision(vng::u64 completed, vng::u64 minimum,
                                                        vng::u64 presented, vng::u64 authored) {
    return completed >= minimum && completed >= presented && completed <= authored;
}
// Backpressure belongs beside the authored document, not in the transport.
// Retain only the latest absolute value of each dirty vertex while one update
// is in flight; never build a queue of historical mouse-move snapshots.
class PreviewUpdates {
public:
    void add(vng::u64 generation);
    void remove(vng::u64 generation);
    void reset(vng::u64 generation);
    // A rejected snapshot waits for a new local edit or explicit reconnect;
    // it must neither block that later edit nor retry invalid data in a loop.
    void reject(vng::u64 generation);
    void changed();
    void changed(const DocumentChanges&);
    void changed(std::span<const vng::u32> vertices, vng::u32 blueprint = 1);
    void playback_changed();
    // Coalesce by target, not the last edit kind. Mixed changes and different
    // objects/blueprints remain one atomic value patch behind an ACK.
    void position_changed(vng::u32 object);
    void rotation_changed(vng::u32 object);
    void scale_changed(vng::u32 object);
    // Selection is transient editor view state, not authored scene content.
    // Multiple pending selections collapse to the latest object/vertex pair.
    void selection_changed();
    void acknowledge(vng::u64 generation, vng::u64 revision);
    // Native callback acknowledgments also establish an authored baseline.
    // Local authoring must be frozen until that callback has acknowledged.
    void accepted(vng::u64 generation, vng::u64 revision);
    [[nodiscard]] bool ready(vng::u64 generation, vng::u64 revision) const;
    // Success reserves one slot until acknowledge(). On transport failure call
    // reset(), which retries as a full snapshot and cannot lose a final edit.
    [[nodiscard]] vng::content::Result<std::optional<std::string>> next(vng::u64 generation,
                                                                        const State&);

private:
    void transform_changed(vng::u32 object, std::string_view);
    struct Peer {
        vng::u64 base{}, sent{};
        DocumentChanges changes{.full = true};
        bool rejected{};
        bool known{}; // Includes intentional revision-only/no-op acknowledgements.
        bool playback{};
        bool selection{};
    };
    std::map<vng::u64, Peer> peers_;
};
} // namespace editor_example
