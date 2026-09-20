#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vng/ui/ui.hpp>

namespace editor_example {
struct SaveRequest {
    std::filesystem::path path;
    bool replace_existing{};
};

// UI-only adapter: the host owns modal input routing and atomic scene writes.
// Poll once after Screen::update(), not in the same frame that opened it. Pass
// raw events to handle consumed Escape and reject held/repeating Enter presses.
// A request leaves the dialog visible; close after success or show error().
class SaveSceneDialog final {
public:
    explicit SaveSceneDialog(vng::ui::Container panel);
    void open(const std::filesystem::path& initial);
    void close();
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] std::optional<SaveRequest> poll(std::span<const vng::input::Event> raw = {});
    void error(std::string_view message);

private:
    vng::ui::Container panel_;
    vng::ui::TextField filename_;
    vng::ui::Label status_;
    vng::ui::Button save_, cancel_;
    std::string last_filename_;
    std::optional<std::filesystem::path> replacement_;
    bool visible_{};
};
} // namespace editor_example
