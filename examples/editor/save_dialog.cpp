#include "save_dialog.hpp"

#include <algorithm>

namespace editor_example {
SaveSceneDialog::SaveSceneDialog(vng::ui::Container panel) : panel_(std::move(panel)) {
    panel_.padding(14).gap(10);
    panel_.label("Save scene as").height(28);
    filename_ = panel_.text_input().height(42).placeholder("Scene path, e.g. scene.vscene");
    status_ = panel_.label("Choose a scene file.").height(48);
    auto buttons = panel_.row().height(40).padding(0).gap(10);
    save_ = buttons.button("Save").width(160);
    cancel_ = buttons.button("Cancel").width(160);
    panel_.visible(false);
}

void SaveSceneDialog::open(const std::filesystem::path& initial) {
    last_filename_ = initial.string();
    filename_.value(last_filename_);
    replacement_.reset();
    save_.text("Save");
    status_.text("Choose a scene file.");
    visible_ = true;
    panel_.visible(true);
    filename_.focus();
}

void SaveSceneDialog::close() {
    visible_ = false;
    replacement_.reset();
    panel_.visible(false);
}

void SaveSceneDialog::error(std::string_view message) {
    replacement_.reset();
    save_.text("Save");
    status_.text(message);
}

std::optional<SaveRequest> SaveSceneDialog::poll(std::span<const vng::input::Event> raw) {
    using namespace vng;
    if (!visible_)
        return {};
    if (cancel_.clicked() || std::ranges::any_of(raw, [](const auto& event) {
            return event.kind == input::EventKind::key_down && event.key == input::Key::escape;
        })) {
        close();
        return {};
    }
    if (filename_.changedText() || filename_.getText() != last_filename_) {
        last_filename_ = filename_.getText();
        replacement_.reset();
        save_.text("Save");
        status_.text("Choose a scene file.");
    }
    const bool enter = filename_.submittedText() &&
                       std::ranges::any_of(raw, [](const auto& event) {
                            return event.kind == input::EventKind::key_down &&
                                   event.key == input::Key::enter && !event.repeat;
                        });
    // Holding Enter must never turn the first replacement prompt into consent.
    if (!save_.clicked() && !enter)
        return {};
    if (last_filename_.empty() || last_filename_.find('\0') != std::string::npos) {
        error("Enter a scene file path.");
        return {};
    }
    const std::filesystem::path path{last_filename_};
    std::error_code code;
    const bool exists = std::filesystem::exists(path, code);
    if (code) {
        error("Cannot check this path: " + code.message());
        return {};
    }
    if (exists && replacement_ != path) {
        replacement_ = path;
        save_.text("Replace");
        status_.text("File exists. Replace it?");
        return {};
    }
    return SaveRequest{path, exists};
}
} // namespace editor_example
