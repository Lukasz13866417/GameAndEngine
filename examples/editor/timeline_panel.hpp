#pragma once

#include "keyframes.hpp"
#include <memory>
#include <span>
#include <string_view>
#include <vng/timeline/timeline.hpp>
#include <vng/editor/selection.hpp>
#include <vng/ui/ui.hpp>

namespace editor_example {
struct TimelineAction {
    enum class Kind { seek, add, edit, apply_range, erase, duration, select_object };
    Kind kind{};
    // Seek/add/erase: timestamp. Edit: source timestamp. Duration: new duration.
    vng::f32 time{};
    vng::f32 destination{}; // Edited timestamp.
    std::string name{};
    std::vector<KeyframeValue> values{};
    std::vector<KeyframeChange> changes{}; // apply_range: time..destination, edited fields only
    std::vector<vng::f32> selection{}; // erase: all selected timestamps
    vng::u64 object{}; // select_object: instance ID or the animation camera
    vng::editor::SelectionMode selection_mode{vng::editor::SelectionMode::replace};
};

// Immutable authoring intents: the application owns validation, undo, revision
// increments and transport. Layer filtering changes this panel only, never playback.
class TimelinePanel final {
public:
    struct Statistics {
        vng::u64 document_refreshes{}, value_refreshes{};
        vng::u64 row_visibility_updates{}, row_layout_updates{};
        friend bool operator==(const Statistics&, const Statistics&) = default;
    };
    [[nodiscard]] Statistics statistics() const;
    TimelinePanel(vng::ui::Container strip, vng::ui::Container list, vng::ui::Container inspector,
                  vng::ui::Container actions, vng::ui::Container range_menu);
    void layout_menu(vng::Vec2 screen_size);
    void actions_enabled(bool);
    [[nodiscard]] bool menu_open() const;
    [[nodiscard]] bool menu_contains(vng::Vec2) const;
    void close_menu();
    ~TimelinePanel();
    TimelinePanel(TimelinePanel&&) noexcept;
    TimelinePanel& operator=(TimelinePanel&&) noexcept;
    TimelinePanel(const TimelinePanel&) = delete;
    TimelinePanel& operator=(const TimelinePanel&) = delete;

    void show(const State&);
    void clear_selection();
    void select_keyframe(vng::f32);
    // Keep the keyframe/draft selected; reveal and focus this object's values.
    void focus_object(vng::u64);
    // Selected groups expand by default; deselected groups collapse. Individual
    // +/- overrides are UI-only and reset when that object's selection changes.
    void selected_objects(std::span<const vng::u64>);
    [[nodiscard]] std::optional<TimelineAction>
    poll(std::span<const vng::input::Event> unhandled = {},
         std::span<const vng::input::Event> raw = {});
    void append(vng::ui::DrawList&) const;
    void compact(bool);
    [[nodiscard]] std::optional<vng::f32> selected_keyframe() const noexcept;
    [[nodiscard]] std::span<const vng::f32> selected_keyframes() const noexcept;
    void select_keyframes(std::span<const vng::f32>);
    [[nodiscard]] bool dragging() const noexcept;
    [[nodiscard]] bool handledPointer() const noexcept;
    void cancel() noexcept;
    // Discard document-specific key drafts after replacing the authored scene.
    void reset() noexcept;
    void error(std::string_view);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace editor_example
