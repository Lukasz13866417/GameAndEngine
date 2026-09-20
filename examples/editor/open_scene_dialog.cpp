#include "open_scene_dialog.hpp"

#include <algorithm>
#include <cctype>

namespace editor_example {
namespace {
constexpr FileDialogSpec scene_spec{
    .title = "Open scene / .vscene projects",
    .placeholder = "Existing .vscene file or directory",
    .listing = "Folders first / .vscene scenes",
    .prompt = "Choose a saved scene (.vscene) to open.",
    .accept = "Open",
    .unopenable = "Cannot open this path. Paste a directory or .vscene path.",
    .empty_directory = "No .vscene files or directories here. Use Up or paste a path.",
    .select = "Select a scene, then Open or Enter. A folder click opens it.",
    .typed = "Press Enter to open this directory or the selected scene.",
    .chosen = "Scene selected. Click Open or press Enter.",
    .missing_path = "Enter an existing .vscene file path.",
    .wrong_kind = "Choose an existing regular .vscene scene.",
    .accepts = is_scene_file,
};
} // namespace

bool is_scene_file(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".vscene";
}

OpenSceneDialog::OpenSceneDialog(vng::ui::Container panel) : FileDialog(std::move(panel), spec()) {}

const FileDialogSpec& OpenSceneDialog::spec() {
    return scene_spec;
}
} // namespace editor_example
