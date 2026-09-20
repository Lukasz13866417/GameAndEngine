#pragma once
#include <vng/ui/ui.hpp>
#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace editor_example {
// A row owns retained control groups. Narrow windows move trailing groups into
// an anchored menu; there are no duplicate widgets or duplicate action handlers.
class ToolbarRow {
public:
    ToolbarRow(vng::ui::Container row, vng::ui::Container menu, std::string_view name)
        : row_(row), menu_(menu), name_(name) {
        row_.padding(0).gap(4).height(36);
        menu_.padding(8).gap(4).scrollbar(vng::ui::ScrollBar::automatic).visible(false);
        menu_.label(name).height(26);
    }
    vng::ui::Container item(vng::f32 width) {
        auto group=row_.column().width(width).height(36).padding(0).gap(0);
        items_.push_back({group,width});
        return group;
    }
    void layout(vng::Vec2 screen) {
        if(!more_) {
            more_host_=row_.column().width(82).height(36).padding(0);
            more_=more_host_.button(name_);
        }
        const auto bounds=row_.bounds();
        vng::f32 total{};
        for(const auto& item:items_) total+=item.width+4;
        const auto room=total-4<=bounds.width ? bounds.width : std::max(0.F,bounds.width-86);
        vng::f32 used{};
        std::size_t visible{};
        for(const auto& item:items_) {
            if(used+item.width>room) break;
            used+=item.width+4; ++visible;
        }
        if(!visible_count_ || *visible_count_!=visible) {
            close();
            for(std::size_t i=0;i<items_.size();++i)
                (i<visible ? row_ : menu_).adopt(items_[i].group);
            row_.adopt(more_host_);
            visible_count_=visible;
        }
        const bool overflow=visible<items_.size();
        more_host_.visible(overflow);
        vng::f32 width=280;
        for(std::size_t i=visible;i<items_.size();++i) width=std::max(width,items_[i].width+38);
        width=std::min(width,std::max(1.F,screen.x-32));
        const auto height=std::min(50.F+40.F*static_cast<vng::f32>(items_.size()-visible),
                                   std::max(36.F,screen.y-bounds.y-bounds.height-12));
        menu_.position({std::clamp(more_host_.bounds().x,0.F,std::max(0.F,screen.x-width-8)),bounds.y+bounds.height+4})
             .width(width).height(height);
    }
    void poll(std::span<const vng::input::Event> events, bool enabled, bool pointer_captured = false) {
        if(!enabled) { close(); return; }
        if(more_ && more_->clicked()) { open_=!open_;menu_.visible(open_); }
        if(open_)
            for(const auto& e:events)
                if(e.kind==vng::input::EventKind::focus_lost ||
                   (e.kind==vng::input::EventKind::key_down && e.key==vng::input::Key::escape) ||
                   // Dropdown options may extend beyond the menu. Let their
                   // captured press/release finish before dismissing it.
                   (((!pointer_captured && e.kind==vng::input::EventKind::pointer_down) || e.kind==vng::input::EventKind::pointer_up) &&
                    !menu_.bounds().contains(e.position) && !more_host_.bounds().contains(e.position)))
                    close();
    }
    void enabled(bool value) { row_.enabled(value);menu_.enabled(value);if(!value)close(); }
    void close() { open_=false;menu_.visible(false); }
    bool opened() const { return open_; }
    // A flyout opened from an overflow item stays anchored to the visible
    // toolbar, even after closing the overflow hides the item's own bounds.
    vng::ui::Rect popup_anchor(const vng::ui::Button& control) const {
        const auto button = control.bounds(), row = row_.bounds();
        if (button.width > 0 && button.y >= row.y && button.y + button.height <= row.y + row.height)
            return button;
        return more_host_.bounds();
    }
private:
    struct Item {vng::ui::Container group;vng::f32 width;};
    vng::ui::Container row_,menu_,more_host_;
    std::string name_;
    std::vector<Item> items_;
    std::optional<vng::ui::Button> more_;
    std::optional<std::size_t> visible_count_;
    bool open_{};
};
}
