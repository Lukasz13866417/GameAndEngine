#pragma once
#include <vng/content/diagnostic.hpp>
#include <vng/core/types.hpp>
#include <string>
#include <string_view>

namespace editor_example {
struct State;
// Absolute latest playback state; scrubbing does not resend mesh/track data.
struct PlaybackEdit {
    vng::u64 base_revision{}, revision{};
    vng::f32 time{};
    bool paused{true};
};
[[nodiscard]] vng::content::Result<PlaybackEdit> playback_edit(vng::u64 base, const State&);
[[nodiscard]] vng::content::Result<std::string> encode_playback_edit(const PlaybackEdit&);
[[nodiscard]] vng::content::Result<PlaybackEdit> decode_playback_edit(std::string_view);
[[nodiscard]] vng::content::Result<void> apply_playback_edit(State&, const PlaybackEdit&);
// Loop both playback modes over the authored duration; pausing leaves the exact
// endpoint available for inspection. A negative/invalid delta cannot rewind.
[[nodiscard]] double advance_playback(double time, double delta, vng::f32 duration);
} // namespace editor_example
