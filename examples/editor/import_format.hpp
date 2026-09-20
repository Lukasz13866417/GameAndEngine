#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>

namespace editor_example {
enum class ImportKind { mesh, effect_preset };
inline std::optional<ImportKind> import_kind(const std::filesystem::path& path) {
    auto extension=path.extension().string();
    std::ranges::transform(extension,extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(extension==".vmesh")return ImportKind::mesh;
    if(extension==".veffect")return ImportKind::effect_preset;
    return {};
}
}
