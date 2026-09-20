#pragma once
#include "../editor/project.hpp"

namespace example::asteroids {
inline constexpr vng::f32 duration=52, reveal_time=38;
inline constexpr vng::u32 hero=1, flagship=3, first_rock=19;
inline constexpr vng::u32 rock_count=600, fleet_count=40;
inline constexpr vng::f32 stop_time=44;
inline bool is_rock(const editor_example::SceneInstance& instance) {
    const auto blueprint=static_cast<vng::u32>(instance.blueprint);
    return blueprint>=6 && blueprint<=8;
}
[[nodiscard]] vng::content::Result<editor_example::State>
author_scene(const std::filesystem::path& asset_directory);
}
