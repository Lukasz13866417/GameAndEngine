#pragma once
#include "../editor/project.hpp"

namespace example::fleet {
inline constexpr vng::f32 duration = 34;
inline constexpr vng::f32 reveal_time = 24;
inline constexpr vng::u32 hero = 1, sun = 2, flagship = 3;
// Offline authoring only. The demo/editor load the resulting ordinary saved
// document; no special motion callbacks are retained or replayed at runtime.
[[nodiscard]] vng::content::Result<editor_example::State>
author_scene(const std::filesystem::path& asset_directory);
} // namespace example::fleet
