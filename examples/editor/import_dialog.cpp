#include "import_dialog.hpp"
#include "import_format.hpp"

namespace editor_example {
namespace {
constexpr FileDialogSpec import_spec{
    .title = "Import / meshes and effect presets",
    .placeholder = "Existing .vmesh / .veffect file or directory",
    .listing = "Folders first / .vmesh meshes / .veffect presets",
    .prompt = "Choose a mesh (.vmesh) or effect preset (.veffect).",
    .accept = "Import",
    .unopenable = "Cannot open this path. Paste a directory, .vmesh or .veffect path.",
    .empty_directory = "No .vmesh / .veffect files or directories here. Use Up or paste a path.",
    .select = "Select a mesh or effect preset, then Import or Enter. A folder click opens it.",
    .typed = "Press Enter to open this directory or import the selected asset.",
    .chosen = "Asset selected. Click Import or press Enter.",
    .missing_path = "Enter an existing .vmesh or .veffect file path.",
    .wrong_kind = "Choose an existing regular .vmesh mesh or .veffect preset.",
    .accepts = [](const std::filesystem::path& path) { return import_kind(path).has_value(); },
};
} // namespace

ImportDialog::ImportDialog(vng::ui::Container panel) : FileDialog(std::move(panel), spec()) {}

const FileDialogSpec& ImportDialog::spec() {
    return import_spec;
}
} // namespace editor_example
