#pragma once
#include "../editor/project.hpp"
namespace example::earth {
inline constexpr vng::u32 instance_id=1;
inline constexpr auto blueprint_id=static_cast<editor_example::BlueprintId>(3);
[[nodiscard]] vng::content::Result<editor_example::State> author_scene(const std::filesystem::path& assets);
}
