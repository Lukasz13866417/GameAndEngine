#pragma once
#include <vng/ui/ui.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

namespace editor_example {
// UI-only range picker. The timeline panel supplies drafts; EditingSession
// owns the atomic edit. This menu never owns or mutates a document.
class KeyframeRangeMenu {
public:
    struct Range { vng::f32 first{}, last{}; };
    explicit KeyframeRangeMenu(vng::ui::Container host) : host_(host) {
        host_.padding(12).gap(8).visible(false).scrollbar(vng::ui::ScrollBar::automatic);
        host_.label("APPLY TO KEYFRAMES").height(26);
        host_.label("Edited fields only / existing keyframes / inclusive range").height(24);
        auto row = host_.row().height(36).padding(0).gap(8);
        row.label("From (s)").width(88); first_ = row.text_input().width(114);
        row.label("To (s)").width(64); last_ = row.text_input().width(114);
        summary_ = host_.label("").height(26);
        message_ = host_.label("").height(48);
        auto actions = host_.row().height(36).padding(0).gap(8);
        confirm_ = actions.button("Apply range").width(180);
        cancel_ = actions.button("Cancel").width(110);
    }
    void open(std::span<const vng::f32> times, Range range) {
        times_.assign(times.begin(), times.end());
        first_.value(format(range.first)); last_.value(format(range.last));
        message_.text("Names and timestamps are not copied.");
        opened_ = true; host_.visible(true); update();
    }
    void close() { opened_ = false; host_.visible(false); }
    bool opened() const { return opened_; }
    bool contains(vng::Vec2 point) const { return opened_ && host_.bounds().contains(point); }
    void error(std::string_view text) { message_.text(text); }
    void layout(vng::Vec2 size, vng::ui::Rect anchor) {
        anchor_ = anchor;
        const auto width = std::min(570.F, std::max(1.F, size.x - 16));
        const auto y = anchor.y + anchor.height + 4;
        host_.position({std::max(8.F, std::min(anchor.x + anchor.width - width, size.x - width - 8)), y})
             .width(width).height(std::max(60.F, std::min(270.F, size.y - y - 8)));
    }
    std::optional<Range> poll(std::span<const vng::input::Event> events) {
        if (!opened_) return {};
        for (const auto& event : events)
            if (event.kind == vng::input::EventKind::focus_lost ||
                (event.kind == vng::input::EventKind::key_down && event.key == vng::input::Key::escape) ||
                (event.kind == vng::input::EventKind::pointer_down && !contains(event.position) && !anchor_.contains(event.position))) {
                close(); return {};
            }
        if (cancel_.clicked()) { close(); return {}; }
        if (first_.changedText() || last_.changedText()) update();
        if (confirm_.clicked() || first_.submittedText() || last_.submittedText()) {
            const auto range = read();
            if (!range || range->first < 0 || range->last < range->first || count(*range) == 0) {
                error("Choose an ordered range containing keyframes."); return {};
            }
            return range;
        }
        return {};
    }
private:
    static std::string format(vng::f32 value) {
        char buffer[64];
        const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
        return error == std::errc{} ? std::string(buffer, end) : std::string{};
    }
    std::optional<Range> read() const {
        auto parse = [](std::string_view text) -> std::optional<vng::f32> {
            vng::f32 value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return {};
            return value;
        };
        const auto first = parse(first_.getText()), last = parse(last_.getText());
        if (!first || !last) return {};
        return Range{*first, *last};
    }
    std::size_t count(Range range) const {
        return std::ranges::count_if(times_, [&](auto time) { return time >= range.first && time <= range.last; });
    }
    void update() {
        const auto range = read();
        summary_.text(range ? std::to_string(count(*range)) + " keyframes in range" : "Enter finite timestamps in seconds.");
    }
    vng::ui::Container host_;
    vng::ui::TextField first_, last_;
    vng::ui::Label summary_, message_;
    vng::ui::Button confirm_, cancel_;
    vng::ui::Rect anchor_;
    std::vector<vng::f32> times_;
    bool opened_{};
};
}
