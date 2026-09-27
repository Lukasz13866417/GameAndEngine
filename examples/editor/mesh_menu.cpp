#include "mesh_menu.hpp"
#include <algorithm>

namespace editor_example {
using namespace vng;
MeshMenu::MeshMenu(ui::Container host) : host_(host) {
    host_.width(264).height(244).padding(8).gap(6).visible(false);
    host_.label("Mesh tools").height(24);
    fill_ = host_.button("Make edge / face (F)").height(34);
    subdivide_ = host_.button("Subdivide").height(34);
    align_ = host_.button("Align to line").height(34);
    hide_ = host_.button("Hide faces (H)").height(34);
    reveal_ = host_.button("Reveal hidden (Alt+H)").height(34);
}
void MeshMenu::open(Vec2 at, Vec2 screen, std::optional<ui::Rect> viewport) {
    if (viewport) at = {viewport->x + 8, viewport->y + std::max(0.F, viewport->height - 252)};
    host_.position({std::clamp(at.x, 0.F, std::max(0.F, screen.x - 264)),
                    std::clamp(at.y, 0.F, std::max(0.F, screen.y - 244))}).visible(true);
    open_ = true;
}
void MeshMenu::close() { open_ = false; host_.visible(false); }
void MeshMenu::received(std::string_view situation, std::size_t selected, const MeshMenuContext& context) {
    situation_ = situation;
    selected_ = selected;
    accepts_input_ = context.accept_input;
    reveal_available_ = context.reveal_available;
}
void MeshMenu::controls(bool fill, bool subdivide, bool align, bool hide, bool reveal) {
    enabled_ = {fill, subdivide, align, hide, reveal};
    fill_.enabled(fill); subdivide_.enabled(subdivide); align_.enabled(align);
    hide_.enabled(hide); reveal_.enabled(reveal);
}
std::optional<MeshAction> MeshMenu::handle(const Vertices& situation, const MeshMenuContext& context) {
    received("Vertices", situation.selected, context);
    controls(situation.selected >= 2, situation.selected != 0, situation.selected != 0,
             situation.selected != 0, context.reveal_available);
    return input(context);
}
std::optional<MeshAction> MeshMenu::handle(const Edges& situation, const MeshMenuContext& context) {
    received("Edges", situation.selected, context);
    controls(situation.selected != 0, situation.selected != 0, situation.selected != 0,
             situation.selected != 0, context.reveal_available);
    return input(context);
}
std::optional<MeshAction> MeshMenu::handle(const Faces& situation, const MeshMenuContext& context) {
    received("Faces", situation.selected, context);
    controls(situation.selected != 0, situation.selected != 0, situation.selected != 0,
             situation.selected != 0, context.reveal_available);
    return input(context);
}
std::optional<MeshAction> MeshMenu::handle(const Inactive&, const MeshMenuContext& context) {
    received("Inactive", 0, context);
    close();
    return {};
}
std::optional<MeshAction> MeshMenu::input(const MeshMenuContext& context) {
    if (!open_ || !context.accept_input) return {};
    // Only this handler consumes activations. Closing on the first result also
    // prevents a later presentation update from repeating a retained click.
    std::optional<MeshAction> action;
    if (enabled_[0] && fill_.clicked()) action = MeshAction::fill;
    if (enabled_[1] && subdivide_.clicked()) action = MeshAction::subdivide;
    if (enabled_[2] && align_.clicked()) action = MeshAction::align;
    if (enabled_[3] && hide_.clicked()) action = MeshAction::hide;
    if (enabled_[4] && reveal_.clicked()) action = MeshAction::reveal;
    if (action) { last_action_ = action; close(); return action; }
    for (const auto& event : context.events)
        if (event.kind == input::EventKind::focus_lost ||
            (event.kind == input::EventKind::key_down && event.key == input::Key::escape) ||
            (event.kind == input::EventKind::pointer_down && !host_.bounds().contains(event.position))) close();
    return {};
}
DebugReport MeshMenu::debug_report() const {
    const auto action_name = [](MeshAction action) -> std::string {
        switch (action) {
        case MeshAction::fill: return "Make edge / face";
        case MeshAction::subdivide: return "Subdivide";
        case MeshAction::align: return "Align to line";
        case MeshAction::hide: return "Hide faces";
        case MeshAction::reveal: return "Reveal hidden";
        }
        return "unknown";
    };
    return {.name = "menu", .role = "mesh operations", .situation = std::string(situation_),
        .received = {{"selected elements", std::to_string(selected_)},
                     {"input allowed", debug_bool(accepts_input_)}, {"hidden faces available", debug_bool(reveal_available_)}},
        .owned = {{"open", debug_bool(open_)}, {"fill enabled", debug_bool(enabled_[0])},
                  {"subdivide enabled", debug_bool(enabled_[1])}, {"align enabled", debug_bool(enabled_[2])}},
        .observations = {{"last requested action (not an execution result)", last_action_ ? action_name(*last_action_) : "none"}}};
}
} // namespace editor_example
