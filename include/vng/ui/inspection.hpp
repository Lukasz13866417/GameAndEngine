#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include <vng/ui/draw_list.hpp>

namespace vng::ui {
// Semantic roles are independent of both the renderer and the UI's storage.
enum class WidgetRole {
    root, column, row, label, button, checkbox, dropdown, text_field, text_area,
    slider, image, option, scrollbar, splitter
};

struct WidgetSnapshot {
    // Opaque screen-local IDs, stable for the widget's lifetime; zero means no
    // parent. Popup options have their own stable ID and parent dropdown ID.
    u64 id{}, parent{};
    WidgetRole role{};
    // The authored name is retained even when a text field contains a value.
    // Labels/buttons use their caption; popup options use the choice caption.
    std::string label;
    // Current text or selected dropdown caption, without decoration such as
    // "label: value". Slider/checkbox values are available in typed fields.
    std::string text, placeholder;
    Rect bounds{}, clip{}; // logical pixels, top-left origin
    // in_layout follows visibility through ancestors. visible additionally
    // requires a positive clipped area, but does not imply lack of occlusion.
    // Hidden nodes have zero bounds/clip; fully clipped nodes retain bounds.
    bool in_layout{}, visible{}, enabled{}; // enabled includes ancestors
    bool hovered{}, pressed{}, focused{}, selected{}, expanded{};
    std::optional<std::size_t> option_index; // only for role == option
    std::optional<f32> number, minimum, maximum; // sliders; splitters/scrollbars use logical pixels
    // A scrollbar is a stable virtual child of its scrolling container. Its
    // bounds are the track, and this is its proportional draggable thumb.
    std::optional<Rect> thumb; // only for scrollbars
    std::optional<bool> checked; // only for checkboxes
};

// Owned read-only data: keeping/modifying a copy never changes the live UI.
// Live widgets (including hidden ones) are in creation order, followed by virtual
// scrollbars and open dropdown options. Options outside a popup's scroll window are hidden.
// Removed widgets and closed dropdown options are omitted.
struct Inspection {
    Vec2 logical_size{};
    Extent2D framebuffer{};
    std::vector<WidgetSnapshot> widgets;
};
} // namespace vng::ui
