#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vng/ui/ui.hpp>

namespace editor_example {
// The words and the accepted file kinds that make one picker differ from
// another. Every message names its own kinds, so a picker never claims to
// accept a file it will refuse.
struct FileDialogSpec {
    std::string_view title, placeholder, listing, prompt, accept;
    std::string_view unopenable, empty_directory, select, typed, chosen, missing_path, wrong_kind;
    bool (*accepts)(const std::filesystem::path&);
};

// UI-only file picker. The host owns modal input routing and whatever it does
// with the chosen path; this component only enumerates directories and returns
// an existing regular file of the accepted kinds. Use a 780 x 560 logical-pixel
// panel. Poll once after Screen::update(), not in the frame that opens the
// dialog. Close after the host succeeds, or show error(). Named pickers such as
// ImportDialog derive to supply their spec; nothing is owned polymorphically.
class FileDialog {
public:
    FileDialog(vng::ui::Container panel, const FileDialogSpec& spec);
    void open(const std::filesystem::path& start = {});
    void close();
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] std::optional<std::filesystem::path>
    poll(std::span<const vng::input::Event> raw = {});
    void error(std::string_view message);

private:
    struct Entry {
        std::filesystem::path path;
        bool directory{};
        vng::ui::Button button;
        std::string label;
    };
    bool browse(const std::filesystem::path&);
    void choose(const std::filesystem::path&);
    void clear_entries();
    void update_selection();
    [[nodiscard]] std::optional<std::filesystem::path> submit();

    FileDialogSpec spec_;
    vng::ui::Container panel_, list_, body_;
    vng::ui::TextField filename_;
    vng::ui::Label status_;
    vng::ui::Button up_, refresh_, accept_, cancel_;
    std::filesystem::path directory_;
    std::optional<std::filesystem::path> selected_;
    std::vector<Entry> entries_;
    std::string last_filename_;
    bool visible_{};
};
} // namespace editor_example
