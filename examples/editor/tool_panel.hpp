#pragma once
#include "tool_options.hpp"
#include "inspector_panel.hpp"
#include "component_debug.hpp"
#include <optional>
#include <string>

namespace editor_example {
// One viewport-owned host. The borrowed tool must outlive this panel (as the
// viewport's interaction children do). Closing options never silently undoes it.
class ToolPanel {
public:
    struct Current {};
    struct Show { ToolOptions& options; bool new_operation{}; };
    struct Context {
        std::optional<vng::ui::Rect> viewport{};
        bool validate{}, poll{}, close{};
    };
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
    [[nodiscard]] DebugReport debug_report() const {
        return {.name="options",.role="retained options for the active viewport tool",
            .situation=opened()?"Showing":"Closed",
            .owned={{"generation",std::to_string(generation_)},{"revision",std::to_string(revision_)},
                {"source revision",std::to_string(source_revision_)},{"dismissed",debug_bool(dismissed_!=nullptr)}},
            .observations={{"status",status_}}};
    }
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }
private:
    friend class EditingViewportUI;
    void handle(const Current&,const Context& c) {
        if(c.close) close();
        if(c.validate) validate();
        if(c.viewport) layout(*c.viewport);
        if(c.poll) poll();
    }
    void handle(const Show& s,const Context& c) {
        show(s.options,s.new_operation);
        handle(Current{},c);
    }
    vng::ui::Container host_,body_;
    vng::ui::Label heading_;
    vng::ui::Button close_;
    InspectorPanel panel_;
    ToolOptions* tool_{};
    ToolOptions* dismissed_{};
    std::optional<vng::editor::Inspector> inspector_;
    vng::u64 generation_{},revision_{};
    vng::u64 source_revision_{};
    std::string status_;
    void refresh();
};
}
