#pragma once

#include "file_dialog.hpp"

namespace editor_example {
// UI-only asset picker for .vmesh meshes and .veffect presets. The host owns
// modal input routing and loading; see FileDialog for the polling contract.
class ImportDialog final : public FileDialog {
public:
    explicit ImportDialog(vng::ui::Container panel);
    [[nodiscard]] static const FileDialogSpec& spec();
};
} // namespace editor_example
