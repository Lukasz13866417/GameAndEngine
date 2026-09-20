#pragma once
#include <vng/timeline/timeline.hpp>

namespace editor_example {
// Transport safety ceiling, separate from the user's authoring budget.
inline constexpr unsigned max_scene_instances = 65536;
inline constexpr unsigned default_instance_limit = 4096;
inline constexpr bool valid_instance_limit(unsigned value) {
    return value > 0 && value <= max_scene_instances;
}
inline constexpr unsigned default_timeline_track_limit = 256;
inline constexpr bool valid_timeline_track_limit(unsigned value) {
    return value > 0 && value <= vng::timeline::max_tracks;
}
} // namespace editor_example
