#include "tool_panel.hpp"
#include <algorithm>

namespace editor_example {
ToolPanel::ToolPanel(vng::ui::Container host)
    : host_(host), body_(host.column().padding(0).gap(4)),
      heading_(body_.label("Tool options").height(24)),
      close_(body_.button("Close tool options").height(28)), panel_(body_) {
    host_.padding(8).gap(4).scrollbar(vng::ui::ScrollBar::automatic).visible(false);
}

void ToolPanel::show(ToolOptions& tool, bool new_operation) {
    if (!new_operation && dismissed_ == &tool) return;
    if (!new_operation && tool_ == &tool && tool.options_available()) return;
    close();
    if (!tool.options_available()) return;
    dismissed_ = nullptr;
    tool_ = &tool;
    ++generation_;
    heading_.text(tool.title());
    host_.visible(true);
    refresh();
}

void ToolPanel::close() {
    if (tool_) dismissed_ = tool_;
    tool_ = nullptr;
    inspector_.reset();
    panel_.clear();
    host_.visible(false);
    status_.clear();
}

void ToolPanel::validate() {
    if (tool_ && !tool_->options_available()) close();
    if (dismissed_ && !dismissed_->options_available()) dismissed_ = nullptr;
}

void ToolPanel::refresh() {
    inspector_.emplace(vng::editor::Stamp{1, generation_, ++revision_});
    tool_->describe_options(*inspector_);
    panel_.show(inspector_->schema());
}

void ToolPanel::poll() {
    status_.clear();
    validate();
    if (!tool_) return;
    if (close_.clicked()) { close(); return; }
    for (const auto& event : panel_.poll()) {
        auto result = inspector_->dispatch(event);
        if (!result) status_ = result.error().message;
        // A callback can end the gesture or undo the operation. Never keep its
        // widgets/callbacks alive for a now-invalid authoring context.
        if (!tool_->options_available()) { close(); return; }
        if (result) refresh();
        break; // Re-described controls have a new stamp; never replay old events.
    }
    if (!panel_.status().empty()) status_ = panel_.status();
}

void ToolPanel::layout(vng::ui::Rect viewport) {
    if (!opened()) return;
    const float width = std::min(360.F, std::max(0.F, viewport.width - 16));
    host_.width(width);
    // Measure the tool's own controls instead of reserving a tall empty panel
    // for simple actions. Large custom menus scroll inside the same host.
    const float height = std::min(body_.bounds().height + 16, std::max(0.F, viewport.height - 32));
    host_.position({viewport.x + 8, viewport.y + viewport.height - height - 8}).height(height);
}
}
