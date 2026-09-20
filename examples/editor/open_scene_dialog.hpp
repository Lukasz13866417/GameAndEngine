#pragma once

#include "file_dialog.hpp"

namespace editor_example {
// A saved editor project; the extension is matched without regard to case.
[[nodiscard]] bool is_scene_file(const std::filesystem::path&);

// UI-only picker for .vscene projects behind the Open scene button. The host
// loads the chosen scene and decides what happens to unsaved work; see
// FileDialog for the polling contract.
class OpenSceneDialog final : public FileDialog {
public:
    explicit OpenSceneDialog(vng::ui::Container panel);
    [[nodiscard]] static const FileDialogSpec& spec();
};
} // namespace editor_example
