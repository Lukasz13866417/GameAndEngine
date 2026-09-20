#pragma once
#include "tool_options.hpp"
#include "inspector_panel.hpp"
#include <optional>
#include <string>

namespace editor_example {
// One viewport-owned host. The borrowed tool must outlive this panel (as the
// viewport's interaction children do). Closing options never silently undoes it.
class ToolPanel {
public:
    explicit ToolPanel(vng::ui::Container);
    void show(ToolOptions&, bool new_operation = false);
    void close();
    void layout(vng::ui::Rect viewport);
    void poll();
    void validate();
    bool opened() const { return tool_!=nullptr; }
    bool showing(const ToolOptions& tool) const {return tool_==&tool;}
    bool contains(vng::Vec2 p) const { return opened() && host_.bounds().contains(p); }
    std::string_view status() const { return status_; }
private:
    vng::ui::Container host_,body_;
    vng::ui::Label heading_;
    vng::ui::Button close_;
    InspectorPanel panel_;
    ToolOptions* tool_{};
    ToolOptions* dismissed_{};
    std::optional<vng::editor::Inspector> inspector_;
    vng::u64 generation_{},revision_{};
    std::string status_;
    void refresh();
};
}
