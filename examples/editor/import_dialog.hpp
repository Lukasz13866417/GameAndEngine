#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vng/ui/ui.hpp>

namespace editor_example {
// UI-only asset picker. The host owns modal input routing and loading; this
// component only enumerates directories and returns a supported file path.
// Use a 780 x 560 logical-pixel panel. Poll once after Screen::update(), not in
// the frame that opens the dialog. Close after a successful import, or error().
class ImportDialog final {
public:
    explicit ImportDialog(vng::ui::Container panel);
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

    vng::ui::Container panel_, list_, body_;
    vng::ui::TextField filename_;
    vng::ui::Label status_;
    vng::ui::Button up_, refresh_, import_, cancel_;
    std::filesystem::path directory_;
    std::optional<std::filesystem::path> selected_;
    std::vector<Entry> entries_;
    std::string last_filename_;
    bool visible_{};
};
} // namespace editor_example
