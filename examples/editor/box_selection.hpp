#pragma once
#include "selection_input.hpp"
#include <cmath>

namespace editor_example {
// Pointer capture only. Picking occurs once on release, not during mouse motion.
class BoxSelection {
public:
    struct Result { vng::ui::Rect rect; vng::editor::SelectionMode mode; };
    void begin(vng::Vec2 point,vng::ui::Rect bounds,vng::input::Modifiers modifiers) {
        start_=end_=point; bounds_=bounds; mode_=box_selection(modifiers); active_=true; dragging_=false;
    }
    void cancel() { active_=dragging_=false; }
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] bool dragging() const { return dragging_; }
    [[nodiscard]] std::optional<Result> update(const vng::input::Event& event) {
        using namespace vng;
        if(!active_) return {};
        if(event.kind==input::EventKind::focus_lost ||
           (event.kind==input::EventKind::key_down && event.key==input::Key::escape)) {cancel();return {};}
        if(event.kind!=input::EventKind::pointer_move &&
           !(event.kind==input::EventKind::pointer_up && event.button==0)) return {};
        if(std::isfinite(event.position.x) && std::isfinite(event.position.y)) {
            end_={std::clamp(event.position.x,bounds_.x,bounds_.x+bounds_.width),
                  std::clamp(event.position.y,bounds_.y,bounds_.y+bounds_.height)};
            dragging_|=std::hypot(end_.x-start_.x,end_.y-start_.y)>=5;
        }
        if(event.kind==input::EventKind::pointer_up) {
            const auto result=dragging_?std::optional{Result{rect(),mode_}}:std::nullopt;
            cancel();return result;
        }
        return {};
    }
    void append(vng::ui::DrawList& draw) const {
        if(dragging_) draw.commands.emplace_back(vng::ui::BoxDraw{rect(),bounds_,
            {.15F,.55F,1,.12F},{.35F,.75F,1,.95F},0,1});
    }
private:
    vng::ui::Rect rect() const {return {std::min(start_.x,end_.x),std::min(start_.y,end_.y),
        std::abs(end_.x-start_.x),std::abs(end_.y-start_.y)};}
    vng::Vec2 start_{},end_{};
    vng::ui::Rect bounds_{};
    vng::editor::SelectionMode mode_{};
    bool active_{},dragging_{};
};
}
