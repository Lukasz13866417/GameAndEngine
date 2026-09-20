#pragma once
#include "camera_limits.hpp"
#include "authoring_limits.hpp"

#include <vng/content/diagnostic.hpp>
#include <vng/window/presentation.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace editor_example {
// Editor preferences, deliberately independent of authored scenes and history.
struct Settings {
    // debug_fps accepts any unsigned rate; zero captures every native Play frame.
    unsigned ui_fps{60}, preview_fps{60}, play_fps{}, debug_fps{10};
    unsigned preview_percent{100};
    OrbitDistanceRange orbit_distance{};
    unsigned timeline_track_limit{default_timeline_track_limit};
    unsigned instance_limit{default_instance_limit};
    vng::window::VSync vsync{vng::window::VSync::on};
    vng::f32 maximum_viewing_distance{10000};
    WalkSpeeds walk{};
    unsigned ui_scale_percent{100};
    CameraDragSpeeds camera_drag{};
    bool scroll_moves_camera{true};
    friend bool operator==(const Settings&, const Settings&) = default;
};
[[nodiscard]] vng::content::Result<void> validate_settings(const Settings&);
[[nodiscard]] std::string encode_settings(const Settings&);
[[nodiscard]] vng::content::Result<Settings> decode_settings(std::string_view);
[[nodiscard]] std::filesystem::path settings_path();
[[nodiscard]] vng::content::Result<Settings> load_settings(const std::filesystem::path&);
[[nodiscard]] vng::content::Result<void> save_settings(const std::filesystem::path&, const Settings&);
// Zero is uncapped. A cap controls wall-clock presentation, never playback speed.
[[nodiscard]] inline std::chrono::steady_clock::duration frame_interval(unsigned fps) {
    return fps ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::duration<double>{1.0 / fps})
               : std::chrono::steady_clock::duration::zero();
}
} // namespace editor_example
