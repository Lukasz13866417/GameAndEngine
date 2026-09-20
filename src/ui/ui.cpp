#include <vng/ui/ui.hpp>
#include "text_edit.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <charconv>
#include <iterator>
#include <list>

namespace vng::ui::detail {
namespace {
Rect intersect(Rect a, Rect b) {
    const auto x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0F, std::min(a.x + a.width, b.x + b.width) - x),
            std::max(0.0F, std::min(a.y + a.height, b.y + b.height) - y)};
}
bool container(Kind k) {
    return k == Kind::root || k == Kind::column || k == Kind::row;
}
bool editable(Kind k) {
    return k == Kind::input || k == Kind::area;
}
bool scalar_control(Kind k) { return k == Kind::slider || k == Kind::splitter; }
bool interactive(Kind k) {
    return !container(k) && k != Kind::label && k != Kind::image;
}
WidgetRole role(Kind kind) {
    switch (kind) {
    case Kind::root: return WidgetRole::root;
    case Kind::column: return WidgetRole::column;
    case Kind::row: return WidgetRole::row;
    case Kind::label: return WidgetRole::label;
    case Kind::button: return WidgetRole::button;
    case Kind::checkbox: return WidgetRole::checkbox;
    case Kind::dropdown: return WidgetRole::dropdown;
    case Kind::input: return WidgetRole::text_field;
    case Kind::area: return WidgetRole::text_area;
    case Kind::slider: return WidgetRole::slider;
    case Kind::image: return WidgetRole::image;
    case Kind::splitter: return WidgetRole::splitter;
    }
    throw std::logic_error("Unknown UI widget kind");
}
void finite(f32 value, f32 low, f32 high) {
    if (!std::isfinite(value) || value < low || value > high)
        throw std::invalid_argument("Invalid UI dimension");
}
void validate_theme(const std::shared_ptr<const Theme>& theme) {
    if (!theme || !theme->values().font)
        throw std::invalid_argument("UI theme needs a loaded font");
    const auto& t = theme->values();
    finite(t.font_size, 1, 256);
    finite(t.padding, 0, 256);
    finite(t.gap, 0, 256);
    finite(t.control_height, 1, 1024);
    finite(t.radius, 0, 256);
    finite(t.border_width, 0, 64);
    finite(t.scrollbar_width, 4, 64);
    finite(t.scrollbar_gap, 0, 64);
    finite(t.scrollbar_min_thumb, 4, 256);
    for (auto color : {t.panel, t.surface, t.hover, t.pressed, t.text, t.muted, t.accent, t.border,
                       t.focus, t.selection, t.scrollbar_track, t.scrollbar_thumb,
                       t.scrollbar_hover, t.scrollbar_pressed})
        for (u32 i = 0; i < 4; ++i)
            finite(color[i], 0, 1);
}
} // namespace
struct Node {
    u64 identity{};
    std::list<u32>::iterator live_entry;
    Kind kind{};
    u32 parent{};
    std::vector<u32> children;
    bool visible{true}, enabled{true}, shown{}, active{};
    bool inspection_enabled{}; // scratch: inherited enabled flag
    bool hovered{}, clicked{}, changed{}, submitted{};
    bool edit_started{}, edit_committed{}, edit_cancelled{}, editing{};
    f32 minimum{}, maximum{1}, value{}, step{}, initial_value{};
    SplitAxis split_axis{SplitAxis::y};
    f32 split_pointer{};
    std::shared_ptr<const gfx::ImageData> pixels;
    std::shared_ptr<const void> image_identity;
    u64 image_revision{};
    Vec2 position{};
    f32 width{-1}, height{-1}, padding{-1}, gap{-1}, scroll{}, max_scroll{};
    ScrollBar scrollbar{ScrollBar::automatic};
    Rect scroll_track{}, scroll_thumb{};
    f32 scroll_viewport{};
    Rect rect{}, clip{};
    std::string text, label, placeholder;
    bool checked{};
    std::vector<std::string> choices;
    std::size_t selected{}, highlighted{}, popup_first{};
    Editor edit;
    // Natural-width queries recur through nested layout passes. Cache shaping,
    // not padding, so theme/font/text changes are still observed precisely.
    struct MeasuredText {
        u64 font{};
        u32 size{};
        std::string text;
        f32 width{};
    };
    mutable std::optional<MeasuredText> measured_text;
    // Choices are immutable for a dropdown's lifetime. Only changing its font
    // or font size requires measuring them again, not selection/hover/draw.
    struct MeasuredChoices {
        u64 font{};
        u32 size{};
        f32 width{};
    };
    std::optional<MeasuredChoices> measured_choices;
    u64 measure_epoch{};
    f32 measured_width{}, measured_height{};
};
struct State {
    std::shared_ptr<const Theme> theme;
    std::vector<std::unique_ptr<Node>> nodes;
    // Slots retain only bounded high-water capacity. Nodes and their resources
    // are destroyed on removal; this list traverses live widgets in creation order.
    std::vector<u32> free_slots;
    std::list<u32> live_nodes;
    u64 next_identity{1};
    std::vector<u32> order, paint_order;
    u64 layout_epoch{};
    Vec2 size{}, scale{1, 1}, pointer{};
    Extent2D framebuffer{};
    bool dirty{true}, focused_window{true}, release_owned{};
    u32 focus{}, capture{}, popup{};
    bool capture_scrollbar{};
    f32 scroll_initial{}, scroll_grab{}, scroll_capture_limit{};
    Rect scroll_capture_track{};
    input::Key pressed_key{input::Key::unknown};
    std::array<bool, static_cast<std::size_t>(input::Key::count)> owned_keys{};
    f32 clock{};
    Node& node(u32 id) { return *nodes.at(id); }
    const ThemeValues& style() const { return theme->values(); }
    bool live(u32 id) const { return id < nodes.size() && nodes[id] != nullptr; }
    bool live(const Handle& h) const { return live(h.id) && nodes[h.id]->identity == h.identity; }
    bool descends_from(u32 id, u32 ancestor) const {
        while (id && id != ancestor) id = nodes[id]->parent;
        return id == ancestor;
    }
    // Visibility/enabled/removal setters invalidate interaction immediately,
    // but don't each trigger a full layout. Bounds, input and drawing flush it.
    void invalidate_interaction(u32 subtree) {
        if (focus && descends_from(focus, subtree)) {
            focus = 0;
            pressed_key = input::Key::unknown;
        }
        if (capture && descends_from(capture, subtree)) {
            if (capture_scrollbar)
                node(capture).scroll = std::clamp(scroll_initial, 0.F, node(capture).max_scroll);
            else cancel_scalar_edit(node(capture));
            capture = 0;
            capture_scrollbar = false;
            release_owned = true;
        }
        if (popup && descends_from(popup, subtree)) popup = 0;
    }
    f32 pad(const Node& n) const { return n.padding < 0 ? style().padding : n.padding; }
    f32 gap(const Node& n) const { return n.gap < 0 ? style().gap : n.gap; }
    f32 natural_width(const Node& n) const {
        if (n.kind == Kind::splitter) return n.split_axis == SplitAxis::x ? 10.F : 100.F;
        if (container(n.kind))
            return 280;
        const auto& theme = style();
        const auto size = static_cast<u32>(std::round(theme.font_size));
        const auto& text = n.label.empty() ? n.text : n.label;
        if (!n.measured_text || n.measured_text->font != theme.font.id() ||
            n.measured_text->size != size || n.measured_text->text != text) {
            const auto measured = theme.font.measure(text, size);
            if (!measured)
                throw std::runtime_error(measured.error().message);
            n.measured_text.emplace(theme.font.id(), size, text, measured->width);
        }
        return std::max(100.0F, n.measured_text->width + pad(n) * 2 + 24);
    }
    f32 measure(u32 id, f32 width) {
        auto& n = node(id);
        if (n.measure_epoch == layout_epoch && n.measured_width == width)
            return n.measured_height;
        const auto height = measure_uncached(id, width);
        n.measure_epoch = layout_epoch;
        n.measured_width = width;
        n.measured_height = height;
        return height;
    }
    f32 measure_uncached(u32 id, f32 width) {
        auto& n = node(id);
        if (n.height >= 0)
            return n.height;
        if (n.kind == Kind::area)
            return 120;
        if (n.kind == Kind::image)
            return 180;
        if (n.kind == Kind::splitter && n.split_axis == SplitAxis::y)
            return 10;
        if (!container(n.kind))
            return style().control_height;
        f32 total{}, maximum{};
        std::size_t count{};
        for (auto child : n.children) {
            auto& c = node(child);
            if (!c.visible)
                continue;
            const auto cw = c.width < 0
                                ? (n.kind == Kind::row ? natural_width(c) : width - 2 * pad(n))
                                : c.width;
            const auto ch = measure(child, std::max(0.0F, cw));
            total += ch;
            maximum = std::max(maximum, ch);
            ++count;
        }
        return 2 * pad(n) + (n.kind == Kind::row
                                 ? maximum
                                 : total + (count ? static_cast<f32>(count - 1) * gap(n) : 0));
    }
    void arrange(u32 id, Rect bounds, Rect clip, bool enabled) {
        auto& n = node(id);
        const bool resized = n.rect.width != bounds.width || n.rect.height != bounds.height;
        n.shown = true;
        n.active = enabled && n.enabled;
        n.rect = bounds;
        n.clip = intersect(bounds, clip);
        if (id)
            order.push_back(id);
        if (id && n.clip.width > 0 && n.clip.height > 0)
            paint_order.push_back(id);
        if (!container(n.kind)) {
            if (resized && focus == id && editable(n.kind))
                reveal(n);
            return;
        }
        const auto p = n.kind == Kind::root ? 0.0F : pad(n);
        Rect inner{bounds.x + p, bounds.y + p, std::max(0.0F, bounds.width - 2 * p),
                   std::max(0.0F, bounds.height - 2 * p)};
        f32 content{}, maximum{};
        std::size_t count{};
        for (auto child : n.children) {
            const auto& c = node(child);
            if (!c.visible)
                continue;
            const auto w =
                c.width < 0 ? (n.kind == Kind::column ? inner.width : natural_width(c)) : c.width;
            const auto h = measure(child, w);
            content += h;
            maximum = std::max(maximum, h);
            ++count;
        }
        if (count)
            content += static_cast<f32>(count - 1) * gap(n);
        n.max_scroll = n.kind == Kind::column ? std::max(0.0F, content - inner.height) : 0;
        n.scroll = std::clamp(n.scroll, 0.0F, n.max_scroll);
        n.scroll_viewport = inner.height;
        n.scroll_track = n.scroll_thumb = {};
        if (n.kind == Kind::column && n.scrollbar != ScrollBar::hidden &&
            (n.max_scroll > 0 || n.scrollbar == ScrollBar::always)) {
            const auto width = std::min(inner.width, style().scrollbar_width);
            n.scroll_track = {inner.x + inner.width - width, inner.y, width, inner.height};
            const auto fraction = content > 0 ? std::min(1.F, inner.height / content) : 1.F;
            const auto height = std::min(inner.height,
                std::max(style().scrollbar_min_thumb, inner.height * fraction));
            const auto travel = inner.height - height;
            n.scroll_thumb = {n.scroll_track.x,
                inner.y + (n.max_scroll > 0 ? n.scroll / n.max_scroll * travel : 0.F), width, height};
            inner.width = std::max(0.F, inner.width - width - style().scrollbar_gap);
        }
        f32 cursor{};
        for (auto child : n.children) {
            auto& c = node(child);
            if (!c.visible)
                continue;
            const auto w = std::max(
                0.0F,
                c.width < 0 ? (n.kind == Kind::column ? inner.width : natural_width(c)) : c.width);
            const auto h = measure(child, w);
            const auto x = inner.x + c.position.x + (n.kind == Kind::row ? cursor : 0);
            const auto y =
                inner.y + c.position.y + (n.kind == Kind::column ? cursor - n.scroll : 0);
            arrange(child, {x, y, w, h}, intersect(clip, inner), n.active);
            cursor += (n.kind == Kind::row ? w : h) + gap(n);
        }
    }
    void layout() {
        if (!dirty)
            return;
        ++layout_epoch;
        for (auto id : live_nodes) {
            node(id).shown = false;
            node(id).active = false;
        }
        order.clear();
        paint_order.clear();
        arrange(0, {0, 0, size.x, size.y}, {0, 0, size.x, size.y}, true);
        dirty = false;
        const auto usable = [&](u32 id) { return live(id) && node(id).shown && node(id).active; };
        if (focus && !usable(focus)) {
            focus = 0;
            pressed_key = input::Key::unknown;
        }
        const auto scroll_usable = [&] {
            if (!capture_scrollbar || !capture || !usable(capture)) return false;
            const auto& n = node(capture);
            const auto clipped = intersect(n.scroll_track, n.clip);
            return n.max_scroll > 0 && clipped.width > 0 && clipped.height > 0 &&
                n.scroll_track.x == scroll_capture_track.x &&
                n.scroll_track.y == scroll_capture_track.y &&
                n.scroll_track.width == scroll_capture_track.width &&
                n.scroll_track.height == scroll_capture_track.height &&
                n.max_scroll == scroll_capture_limit;
        };
        if (capture && (!usable(capture) || (capture_scrollbar && !scroll_usable()))) {
            if (capture_scrollbar) {
                node(capture).scroll = std::clamp(scroll_initial, 0.F, node(capture).max_scroll);
                dirty = true;
            } else cancel_scalar_edit(node(capture));
            capture = 0;
            capture_scrollbar = false;
            release_owned = true;
        }
        if (popup && !usable(popup))
            popup = 0;
        if (dirty) layout();
    }
    bool scroll_point(const Node& n, Vec2 p) const {
        return n.scroll_track.width > 0 && n.scroll_track.height > 0 &&
               n.scroll_track.contains(p) && n.clip.contains(p);
    }
    void point_scrollbar(Node& n) {
        const auto travel = n.scroll_track.height - n.scroll_thumb.height;
        if (travel <= 0) return;
        const auto offset = pointer.y - n.scroll_track.y - scroll_grab;
        // A thumb placed exactly at an endpoint can round a few ulps short.
        // Snap that subpixel residue, otherwise the next wheel event is spent
        // reaching the inner endpoint instead of bubbling to its parent.
        const auto fraction = offset <= .001F ? 0.F : offset >= travel - .001F ? 1.F : offset / travel;
        const auto next = std::clamp(fraction, 0.F, 1.F) * n.max_scroll;
        if (next == n.scroll) return;
        n.scroll = next;
        dirty = true;
        layout();
    }
    void cancel_capture() {
        if (!capture) return;
        if (capture_scrollbar) {
            node(capture).scroll = std::clamp(scroll_initial, 0.F, node(capture).max_scroll);
            dirty = true;
        } else cancel_scalar_edit(node(capture));
        capture = 0;
        capture_scrollbar = false;
        layout();
    }
    void scalar_value(Node& n, f32 v) {
        if (n.step > 0 && v > n.minimum && v < n.maximum) {
            const auto steps = std::round((static_cast<double>(v) - n.minimum) / n.step);
            v = static_cast<f32>(static_cast<double>(n.minimum) + steps * n.step);
        }
        v = std::clamp(v, n.minimum, n.maximum);
        if (v != n.value) {
            n.value = v;
            n.changed = true;
        }
    }
    void begin_scalar_edit(Node& n) {
        if (n.editing)
            return;
        n.initial_value = n.value;
        n.editing = n.edit_started = true;
    }
    void cancel_scalar_edit(Node& n) {
        if (!n.editing)
            return;
        if (n.value != n.initial_value) {
            n.value = n.initial_value;
            n.changed = true;
        }
        n.editing = false;
        n.edit_cancelled = true;
    }
    void commit_scalar_edit(Node& n) {
        if (!n.editing)
            return;
        n.editing = false;
        n.edit_committed = true;
    }
    void point_scalar(Node& n) {
        if (n.kind == Kind::splitter) {
            const auto coordinate = n.split_axis == SplitAxis::y ? pointer.y : pointer.x;
            scalar_value(n, n.initial_value + coordinate - n.split_pointer);
            return;
        }
        const auto inset = std::min(pad(n), n.rect.width * .5F);
        const auto width = n.rect.width - 2 * inset;
        if (width > 0)
            scalar_value(n, n.minimum +
                                std::clamp((pointer.x - n.rect.x - inset) / width, 0.0F, 1.0F) *
                                    (n.maximum - n.minimum));
    }
    void prepare(Node& n) {
        if (!n.edit.dirty)
            return;
        n.edit.geometry = geometry(n.text, style(), scale);
        n.edit.dirty = false;
    }
    Rect text_rect(const Node& n) const {
        const auto inset = pad(n);
        const auto vertical =
            std::min(inset, std::max(0.0F, (n.rect.height - n.edit.geometry.line_height) * .5F));
        return {n.rect.x + inset, n.rect.y + vertical, std::max(0.0F, n.rect.width - 2 * inset),
                std::max(0.0F, n.rect.height - 2 * vertical)};
    }
    void clamp_text_scroll(Node& n, Rect area) {
        auto& e = n.edit;
        // A field can receive focus before its first full-sized layout. Growing
        // it must reveal the beginning again when the entire content now fits.
        // Reserve the same two pixels used below to keep an end caret visible.
        e.scroll_x = std::clamp(e.scroll_x, 0.0F,
                                std::max(0.0F, e.geometry.width + 2 - area.width));
        e.scroll_y = std::clamp(e.scroll_y, 0.0F,
                                std::max(0.0F, e.geometry.height - area.height));
    }
    void reveal(Node& n) {
        prepare(n);
        const auto area = text_rect(n);
        const auto p = n.edit.at(n.edit.caret);
        auto& e = n.edit;
        if (p.x < e.scroll_x)
            e.scroll_x = p.x;
        else if (p.x + 2 > e.scroll_x + area.width)
            e.scroll_x = p.x + 2 - area.width;
        if (p.y < e.scroll_y)
            e.scroll_y = p.y;
        else if (p.y + e.geometry.line_height > e.scroll_y + area.height)
            e.scroll_y = p.y + e.geometry.line_height - area.height;
        clamp_text_scroll(n, area);
    }
    Rect popup_rect() {
        if (!popup)
            return {};
        auto& n = node(popup);
        const auto& theme = style();
        const auto font_size = static_cast<u32>(std::round(theme.font_size));
        if (!n.measured_choices || n.measured_choices->font != theme.font.id() ||
            n.measured_choices->size != font_size) {
            f32 widest{};
            for (const auto& choice : n.choices) {
                const auto measured = theme.font.measure(choice, font_size);
                if (!measured) throw std::runtime_error(measured.error().message);
                widest = std::max(widest, measured->width);
            }
            n.measured_choices.emplace(theme.font.id(), font_size, widest);
        }
        const auto width = std::min(size.x,
            std::max(n.rect.width, n.measured_choices->width + 2 * theme.padding));
        const auto height =
            std::min(static_cast<f32>(n.choices.size()), 8.0F) * theme.control_height;
        const auto below_origin = std::clamp(n.rect.y + n.rect.height, 0.F, size.y);
        const auto above_end = std::clamp(n.rect.y, 0.F, size.y);
        const auto below = size.y - below_origin, above = above_end;
        const auto h = std::min(height, std::max(below, above));
        return {std::clamp(n.rect.x, 0.0F, size.x - width),
                below >= height || below >= above ? below_origin : above_end - h,
                width, h};
    }
    u32 hit(Vec2 p) {
        if (popup && popup_rect().contains(p))
            return popup;
        for (auto it = paint_order.rbegin(); it != paint_order.rend(); ++it) {
            const auto& n = node(*it);
            if (n.clip.contains(p) && n.kind != Kind::label)
                return *it;
        }
        return 0;
    }
    void activate(u32 id) {
        auto& n = node(id);
        n.clicked = true;
        if (n.kind == Kind::checkbox) {
            n.checked = !n.checked;
            n.changed = true;
        }
        if (n.kind == Kind::dropdown) {
            if (popup == id)
                popup = 0;
            else {
                popup = id;
                n.highlighted = n.selected;
                n.popup_first = n.selected > 7 ? n.selected - 7 : 0;
            }
        }
    }
    void choose(std::size_t index) {
        auto& n = node(popup);
        index = std::min(index, n.choices.size() - 1);
        if (n.selected != index) {
            n.selected = index;
            n.changed = true;
        }
        popup = 0;
    }
    void set_focus(u32 id) {
        focus = id;
        pressed_key = input::Key::unknown;
        clock = 0;
        // Keyboard navigation also reveals widgets inside scroll containers.
        for (auto parent = id ? node(id).parent : 0; parent; parent = node(parent).parent) {
            auto& n = node(parent);
            if (!n.max_scroll)
                continue;
            const auto top = n.rect.y + pad(n), bottom = n.rect.y + n.rect.height - pad(n);
            const auto r = node(id).rect;
            const auto delta = r.y < top ? r.y - top : std::max(0.0F, r.y + r.height - bottom);
            n.scroll = std::clamp(n.scroll + delta, 0.0F, n.max_scroll);
            dirty = true;
            layout();
        }
        if (id && editable(node(id).kind))
            reveal(node(id));
    }
    void replace(Node& n, std::string_view insertion) {
        auto value = normalize_text(insertion, n.kind == Kind::area);
        const auto lo = std::min(n.edit.caret, n.edit.anchor),
                   hi = std::max(n.edit.caret, n.edit.anchor);
        if (n.text.size() - (hi - lo) + value.size() > 65536)
            return;
        auto text = n.text.substr(0, lo) + value + n.text.substr(hi);
        if (text == n.text)
            return;
        n.edit.remember(n.text);
        n.text = std::move(text);
        n.edit.caret = lo + value.size();
        // Inserting a combining mark can join an adjacent grapheme.
        const auto stops = boundaries(n.text);
        auto next = std::lower_bound(stops.begin(), stops.end(), n.edit.caret);
        n.edit.caret = next == stops.end() ? n.text.size() : *next;
        n.edit.anchor = n.edit.caret;
        n.edit.dirty = true;
        n.changed = true;
        clock = 0;
        reveal(n);
    }
    bool edit_key(Node& n, const input::Event& event, input::Clipboard* clipboard) {
        using input::Key;
        auto& e = n.edit;
        prepare(n);
        const auto key = event.key;
        const auto ctrl = event.modifiers.control || event.modifiers.super;
        const auto shift = event.modifiers.shift;
        if (ctrl && key == Key::a) {
            e.anchor = 0;
            e.caret = n.text.size();
            clock = 0;
            reveal(n);
            return true;
        }
        if (ctrl && (key == Key::c || key == Key::x)) {
            if (clipboard && e.anchor != e.caret) {
                clipboard->write(std::string_view(n.text).substr(std::min(e.anchor, e.caret),
                                                                 std::max(e.anchor, e.caret) -
                                                                     std::min(e.anchor, e.caret)));
                if (key == Key::x)
                    replace(n, {});
            }
            return true;
        }
        if (ctrl && key == Key::v) {
            if (clipboard)
                replace(n, clipboard->read());
            return true;
        }
        if (ctrl && (key == Key::z || key == Key::y)) {
            const bool redo = key == Key::y || shift;
            auto& from = redo ? e.redo : e.undo;
            auto& to = redo ? e.undo : e.redo;
            if (!from.empty()) {
                to.push_back({n.text, e.caret, e.anchor});
                auto saved = std::move(from.back());
                from.pop_back();
                n.text = std::move(saved.text);
                e.caret = saved.caret;
                e.anchor = saved.anchor;
                e.dirty = true;
                n.changed = true;
                reveal(n);
            }
            return true;
        }
        if (key == Key::backspace || key == Key::del) {
            if (e.anchor == e.caret) {
                if (key == Key::backspace)
                    e.anchor = e.previous();
                else
                    e.anchor = e.next(n.text.size());
            }
            replace(n, {});
            return true;
        }
        if (key == Key::enter) {
            if (n.kind == Kind::area && !ctrl)
                replace(n, "\n");
            else
                n.submitted = true;
            return true;
        }
        auto destination = e.caret;
        if (key == Key::left)
            destination =
                !shift && e.anchor != e.caret ? std::min(e.anchor, e.caret) : e.previous();
        else if (key == Key::right)
            destination =
                !shift && e.anchor != e.caret ? std::max(e.anchor, e.caret) : e.next(n.text.size());
        else if (key == Key::home) {
            auto start = e.caret ? n.text.rfind('\n', e.caret - 1) : std::string::npos;
            destination = ctrl || start == std::string::npos ? 0 : start + 1;
        } else if (key == Key::end) {
            auto end = n.text.find('\n', e.caret);
            destination = ctrl || end == std::string::npos ? n.text.size() : end;
        } else if (key == Key::up || key == Key::down) {
            auto p = e.at(e.caret);
            p.y += key == Key::up ? -e.geometry.line_height : e.geometry.line_height;
            destination = e.nearest(p);
        } else
            return false;
        e.caret = destination;
        if (!shift)
            e.anchor = destination;
        clock = 0;
        reveal(n);
        return true;
    }
    bool event(const input::Event& event, input::Clipboard* clipboard) {
        using input::EventKind;
        using input::Key;
        const auto kind = event.kind;
        if (kind == EventKind::focus_lost) {
            focused_window = false;
            cancel_capture();
            set_focus(0);
            capture = popup = 0;
            release_owned = false;
            owned_keys.fill(false);
            return false;
        }
        if (kind == EventKind::focus_gained) {
            focused_window = true;
            return false;
        }
        if (!focused_window)
            return false;
        if (kind == EventKind::pointer_move || kind == EventKind::pointer_down ||
            kind == EventKind::pointer_up || kind == EventKind::scroll)
            pointer = event.position;
        const auto target = hit(pointer);
        const bool image_target = target && node(target).kind == Kind::image;
        if (kind == EventKind::scroll) {
            if (capture_scrollbar || (capture && node(capture).kind == Kind::splitter)) return true;
            if (popup && popup_rect().contains(pointer)) {
                auto& n = node(popup);
                const auto max = n.choices.size() > 8 ? n.choices.size() - 8 : 0;
                n.popup_first = static_cast<std::size_t>(std::clamp(
                    static_cast<f32>(n.popup_first) - event.scroll.y, 0.0F, static_cast<f32>(max)));
                return true;
            }
            for (auto id = target; id; id = node(id).parent) {
                auto& n = node(id);
                if (n.active && n.kind == Kind::area) {
                    prepare(n);
                    const auto next =
                        std::clamp(n.edit.scroll_y - event.scroll.y * style().control_height, 0.0F,
                                   std::max(0.0F, n.edit.geometry.height - text_rect(n).height));
                    if (next != n.edit.scroll_y) {
                        n.edit.scroll_y = next;
                        return true;
                    }
                }
                if (n.active && n.max_scroll > 0) {
                    const auto next = std::clamp(n.scroll - event.scroll.y * style().control_height,
                                                 0.0F, n.max_scroll);
                    if (next != n.scroll) {
                        n.scroll = next;
                        dirty = true;
                        layout();
                        return true;
                    }
                }
            }
            return target != 0 && !image_target;
        }
        if (kind == EventKind::pointer_down && event.button == 0) {
            if (popup) {
                if (popup_rect().contains(pointer)) {
                    capture = popup;
                    release_owned = true;
                    return true;
                }
                if (target != popup) {
                    popup = 0;
                    release_owned = true;
                    return true;
                } // dismiss without click-through
            }
            if (!target) {
                set_focus(0);
                return false;
            }
            if (image_target) {
                set_focus(0);
                return false;
            }
            auto& n = node(target);
            release_owned = true;
            if (n.active && n.max_scroll > 0 && scroll_point(n, pointer)) {
                // A pending Space/Enter activation belongs to the old keyboard
                // interaction, not to this pointer gesture.
                pressed_key = Key::unknown;
                capture = target;
                capture_scrollbar = true;
                scroll_initial = n.scroll;
                scroll_capture_track = n.scroll_track;
                scroll_capture_limit = n.max_scroll;
                scroll_grab = n.scroll_thumb.contains(pointer) ? pointer.y - n.scroll_thumb.y
                                                                : n.scroll_thumb.height * .5F;
                point_scrollbar(n);
                return true;
            }
            if (!n.active || !interactive(n.kind)) {
                set_focus(0);
                return true;
            }
            set_focus(target);
            capture = target;
            if (scalar_control(n.kind)) {
                begin_scalar_edit(n);
                n.split_pointer = n.split_axis == SplitAxis::y ? pointer.y : pointer.x;
                point_scalar(n);
            }
            if (editable(n.kind)) {
                prepare(n);
                const auto r = text_rect(n);
                n.edit.caret = n.edit.nearest(
                    {pointer.x - r.x + n.edit.scroll_x, pointer.y - r.y + n.edit.scroll_y});
                if (!event.modifiers.shift)
                    n.edit.anchor = n.edit.caret;
            }
            return true;
        }
        if (kind == EventKind::pointer_move) {
            if (capture && capture_scrollbar)
                point_scrollbar(node(capture));
            else if (capture && scalar_control(node(capture).kind))
                point_scalar(node(capture));
            if (capture && editable(node(capture).kind)) {
                auto& n = node(capture);
                const auto r = text_rect(n);
                n.edit.caret = n.edit.nearest(
                    {pointer.x - r.x + n.edit.scroll_x, pointer.y - r.y + n.edit.scroll_y});
                reveal(n);
            }
            return capture || (target && !image_target);
        }
        if (kind == EventKind::pointer_up && event.button == 0) {
            const auto owned = release_owned;
            release_owned = false;
            const auto pressed = capture;
            const auto scrolled = capture_scrollbar;
            if (pressed && scrolled) point_scrollbar(node(pressed));
            capture = 0;
            capture_scrollbar = false;
            if (pressed && scrolled) {
                return owned;
            } else if (pressed && scalar_control(node(pressed).kind)) {
                point_scalar(node(pressed));
                commit_scalar_edit(node(pressed));
            } else if (pressed && pressed == popup && popup_rect().contains(pointer)) {
                choose(node(popup).popup_first +
                       static_cast<std::size_t>((pointer.y - popup_rect().y) /
                                                style().control_height));
            } else if (pressed && target == pressed && node(pressed).active &&
                       !editable(node(pressed).kind))
                activate(pressed);
            return owned;
        }
        // Scrolling is a complete gesture: typing must not edit a retained
        // field, activate its old button, or leak Delete to the application.
        // Keep focus where it was; Tab may navigate after the gesture ends.
        if (capture_scrollbar && (kind == EventKind::text || kind == EventKind::key_down ||
                                  kind == EventKind::key_up)) {
            if (kind == EventKind::key_up) {
                const auto index = static_cast<std::size_t>(event.key);
                if (index < owned_keys.size()) owned_keys[index] = false;
            }
            if (kind == EventKind::key_down && event.key == Key::escape)
                cancel_capture();
            return true;
        }
        if (kind == EventKind::text) {
            if (focus && editable(node(focus).kind)) {
                replace(node(focus), event.text);
                return true;
            }
            return focus != 0;
        }
        if (kind == EventKind::key_up) {
            const auto index = static_cast<std::size_t>(event.key);
            const auto owned = index < owned_keys.size() && std::exchange(owned_keys[index], false);
            if (pressed_key == event.key && focus) {
                const auto id = focus;
                pressed_key = Key::unknown;
                activate(id);
            }
            return owned || focus || popup;
        }
        if (kind != EventKind::key_down)
            return target != 0 && !image_target;
        if (event.key == Key::tab) {
            if (capture && scalar_control(node(capture).kind)) {
                commit_scalar_edit(node(capture));
                capture = 0;
            }
            popup = 0;
            std::vector<u32> stops;
            for (auto id : order)
                if (node(id).active && interactive(node(id).kind))
                    stops.push_back(id);
            if (stops.empty())
                return false;
            auto it = std::find(stops.begin(), stops.end(), focus);
            const auto current = it == stops.end() ? (event.modifiers.shift ? 0 : stops.size() - 1)
                                                   : static_cast<std::size_t>(it - stops.begin());
            const auto next = event.modifiers.shift ? (current + stops.size() - 1) % stops.size()
                                                    : (current + 1) % stops.size();
            set_focus(stops[next]);
            return true;
        }
        if (event.key == Key::escape) {
            if (capture && scalar_control(node(capture).kind)) {
                cancel_scalar_edit(node(capture));
                capture = 0;
                return true;
            }
            if (popup) {
                popup = 0;
                return true;
            }
            if (focus) {
                if (editable(node(focus).kind))
                    node(focus).edit_cancelled = true;
                set_focus(0);
                return true;
            }
            return false;
        }
        if (!focus)
            return false;
        auto& n = node(focus);
        // Ordinary controls have no text clipboard/undo history. Keep their
        // focus (e.g. an inspector entry revealed by selection), but let the
        // owning application handle editing shortcuts. Text and open popups
        // still get first refusal.
        const bool edit_shortcut = event.key == Key::c || event.key == Key::v ||
            event.key == Key::z || event.key == Key::y;
        if (edit_shortcut && event.modifiers.control && !event.modifiers.alt &&
            !event.modifiers.super && !editable(n.kind) && !popup && !capture)
            return false;
        // Let applications delete their selected scene/list item when focus is
        // on an ordinary control. Text editing and active gestures/popups retain
        // the key so a document-level shortcut cannot accidentally destroy it.
        if (event.key == Key::del && !editable(n.kind) && !popup && !capture &&
            !event.modifiers.control && !event.modifiers.super && !event.modifiers.alt &&
            !event.modifiers.shift)
            return false;
        if (event.key == Key::del && !editable(n.kind))
            return true; // Modified Delete is not the plain selection-deletion action.
        if (scalar_control(n.kind)) {
            const auto key = event.key;
            if (key == Key::left || key == Key::right || key == Key::up || key == Key::down ||
                key == Key::home || key == Key::end || key == Key::page_up ||
                key == Key::page_down) {
                const bool dragging = n.editing;
                begin_scalar_edit(n);
                const auto step = n.kind == Kind::splitter ? 1.F : n.step > 0 ? n.step : (n.maximum - n.minimum) * .01F;
                const auto amount =
                    step * ((event.modifiers.shift || key == Key::page_up || key == Key::page_down)
                                ? 10.0F
                                : 1.0F);
                const bool decrease = n.kind == Kind::splitter
                    ? (key == Key::left || key == Key::up || key == Key::page_up)
                    : (key == Key::left || key == Key::down || key == Key::page_down);
                auto next = n.value + (decrease ? -amount : amount);
                if (key == Key::home)
                    next = n.minimum;
                if (key == Key::end)
                    next = n.maximum;
                scalar_value(n, next);
                if (!dragging)
                    commit_scalar_edit(n);
                return true;
            }
            return popup || capture;
        }
        if (n.kind == Kind::dropdown && (event.key == Key::up || event.key == Key::down ||
                                         event.key == Key::enter || event.key == Key::space)) {
            if (event.repeat && (event.key == Key::enter || event.key == Key::space))
                return true;
            if (!popup) {
                activate(focus);
                return true;
            }
            if (event.key == Key::enter || event.key == Key::space) {
                choose(n.highlighted);
                return true;
            }
            if (event.key == Key::up && n.highlighted)
                --n.highlighted;
            if (event.key == Key::down && n.highlighted + 1 < n.choices.size())
                ++n.highlighted;
            if (n.highlighted < n.popup_first)
                n.popup_first = n.highlighted;
            if (n.highlighted >= n.popup_first + 8)
                n.popup_first = n.highlighted - 7;
            return true;
        }
        if (editable(n.kind)) {
            edit_key(n, event, clipboard);
            return true;
        }
        if (event.key == Key::enter || event.key == Key::space) {
            if (!event.repeat) pressed_key = event.key;
            return true;
        }
        // A focused ordinary control owns its activation/navigation keys, not
        // arbitrary application shortcuts. Text editors and popups stay exclusive.
        return popup != 0;
    }
    DrawList paint() {
        layout();
        DrawList list{size, framebuffer, {}};
        const auto& t = style();
        const auto box = [&](Rect r, Rect clip, Vec4 fill, Vec4 border = {}, f32 radius = 0,
                             f32 stroke = 0) {
            if (intersect(r, clip).width > 0 && intersect(r, clip).height > 0)
                list.commands.emplace_back(BoxDraw{r, clip, fill, border, radius, stroke});
        };
        const auto text = [&](std::string_view value, Vec2 at, Rect clip, Vec4 color) {
            if (!value.empty() && clip.width > 0 && clip.height > 0)
                list.commands.emplace_back(
                    TextDraw{std::string(value), at, clip, t.font, t.font_size, color});
        };
        for (auto id : paint_order) {
            auto& n = node(id);
            const auto r = n.rect, clip = n.clip;
            if (clip.width <= 0 || clip.height <= 0)
                continue;
            const auto ink = n.active ? t.text : t.muted;
            if (container(n.kind)) {
                box(r, clip, t.panel, t.border, t.radius, t.border_width);
                if (n.scroll_track.width > 0 && n.scroll_track.height > 0) {
                    const auto track = n.scroll_track;
                    box(track, clip, t.scrollbar_track, {}, track.width * .5F);
                    auto thumb = n.scroll_thumb;
                    const auto inset = std::min(2.F, thumb.width * .2F);
                    thumb.x += inset;
                    thumb.width -= 2 * inset;
                    const auto fill = !n.active || !n.max_scroll ? t.muted
                        : capture == id && capture_scrollbar ? t.scrollbar_pressed
                        : focused_window && scroll_point(n, pointer) ? t.scrollbar_hover
                        : t.scrollbar_thumb;
                    box(thumb, clip, fill, {}, thumb.width * .5F);
                }
                continue;
            }
            const auto line = t.font_size * 1.25F;
            Vec2 at{r.x + pad(n), r.y + std::max(0.0F, (r.height - line) * .5F)};
            if (n.kind == Kind::label) {
                text(n.text, at, clip, ink);
                continue;
            }
            if (n.kind == Kind::image) {
                if (n.pixels)
                    list.commands.emplace_back(
                        ImageDraw{r, clip, n.pixels, n.image_identity, n.image_revision});
                continue;
            }
            if (n.kind == Kind::splitter) {
                const auto fill = !n.active ? t.panel : capture == id ? t.pressed : n.hovered ? t.hover : t.panel;
                const auto ink = !n.active ? t.muted : capture == id || n.hovered || focus == id ? t.focus : t.border;
                box(r, clip, fill);
                if (n.split_axis == SplitAxis::y) {
                    box({r.x, r.y + r.height*.5F - .5F, r.width, 1}, clip, t.border);
                    const auto grip = std::min(40.F, r.width);
                    box({r.x + (r.width-grip)*.5F, r.y + r.height*.5F - 1.5F, grip, 3}, clip, ink, {}, 1);
                } else {
                    box({r.x + r.width*.5F - .5F, r.y, 1, r.height}, clip, t.border);
                    const auto grip = std::min(40.F, r.height);
                    box({r.x + r.width*.5F - 1.5F, r.y + (r.height-grip)*.5F, 3, grip}, clip, ink, {}, 1);
                }
                continue;
            }
            const auto fill = !n.active ? t.panel
                              : capture == id || (focus == id && pressed_key != input::Key::unknown)
                                  ? t.pressed
                              : n.kind == Kind::button && n.checked ? t.pressed
                              : n.hovered ? t.hover
                                          : t.surface;
            box(r, clip, fill, focus == id ? t.focus : t.border, t.radius,
                focus == id ? std::max(2.0F, t.border_width) : t.border_width);
            if (n.kind == Kind::slider) {
                const auto inset = std::min(pad(n), r.width * .5F);
                const auto width = r.width - 2 * inset;
                const auto fraction = (n.value - n.minimum) / (n.maximum - n.minimum);
                box({r.x + inset, r.y + r.height - 7, width, 3}, clip, t.border);
                box({r.x + inset, r.y + r.height - 7, width * fraction, 3}, clip, t.accent);
                box({r.x + inset + width * fraction - 3, r.y + r.height - 10, 6, 9}, clip,
                    n.active ? t.focus : t.muted, {}, 2);
                std::array<char, 32> number{};
                const auto [end, error] =
                    std::to_chars(number.data(), number.data() + number.size(), n.value,
                                  std::chars_format::general, 5);
                const auto value = error == std::errc{} ? std::string(number.data(), end) : "?";
                at.y = r.y + std::max(0.0F, (r.height - line - 8) * .5F);
                text(n.label + ": " + value, at, clip, ink);
            } else if (n.kind == Kind::checkbox) {
                const Rect check{r.x + pad(n), r.y + (r.height - 18) * .5F, 18, 18};
                box(check, clip, n.checked ? t.accent : t.panel, t.border, 3, 1);
                if (n.checked)
                    box({check.x + 5, check.y + 5, 8, 8}, clip, t.text);
                at.x += 28;
                text(n.label, at, clip, ink);
            } else if (n.kind == Kind::dropdown) {
                text(n.label + ": " + n.choices[n.selected], at,
                     intersect(clip, {r.x, r.y, std::max(0.0F, r.width - 24), r.height}), ink);
                text(popup == id ? "−" : "+", {r.x + r.width - 22, at.y}, clip, t.muted);
            } else if (editable(n.kind)) {
                prepare(n);
                const auto area = text_rect(n);
                clamp_text_scroll(n, area);
                const auto content_clip = intersect(clip, area);
                auto& e = n.edit;
                const Vec2 origin{area.x - e.scroll_x, area.y - e.scroll_y};
                if (focus == id && e.anchor != e.caret) {
                    const auto lo = std::min(e.anchor, e.caret), hi = std::max(e.anchor, e.caret);
                    for (std::size_t i = 0; i + 1 < e.geometry.carets.size(); ++i) {
                        const auto a = e.geometry.carets[i], b = e.geometry.carets[i + 1];
                        if (a.byte < lo || a.byte >= hi)
                            continue;
                        const auto width = b.position.y == a.position.y
                                               ? std::abs(b.position.x - a.position.x)
                                               : 5;
                        box({origin.x + std::min(a.position.x, b.position.y == a.position.y
                                                                   ? b.position.x
                                                                   : a.position.x),
                             origin.y + a.position.y, width, e.geometry.line_height},
                            content_clip, t.selection);
                    }
                }
                text(n.text.empty() ? (n.placeholder.empty() ? n.label : n.placeholder) : n.text,
                     origin, content_clip, n.text.empty() ? t.muted : ink);
                if (focus == id && std::fmod(clock, 1.0F) < .65F) {
                    const auto caret = e.at(e.caret);
                    box({origin.x + caret.x, origin.y + caret.y, 1.5F, e.geometry.line_height},
                        content_clip, t.text);
                }
            } else
                text(n.text, at, clip, ink);
        }
        if (popup) {
            auto& n = node(popup);
            const auto r = popup_rect();
            box(r, {0, 0, size.x, size.y}, t.surface, t.focus, t.radius, 1);
            const auto count = std::min<std::size_t>(8, n.choices.size() - n.popup_first);
            for (std::size_t i = 0; i < count; ++i) {
                const Rect row{r.x, r.y + static_cast<f32>(i) * t.control_height, r.width,
                               t.control_height};
                const auto index = n.popup_first + i;
                if (row.contains(pointer) || index == n.highlighted)
                    box(row, r, t.hover);
                text(n.choices[index],
                     {row.x + t.padding, row.y + (row.height - t.font_size * 1.25F) * .5F}, r,
                     index == n.selected ? t.focus : t.text);
            }
        }
        return list;
    }
    Inspection inspect() {
        layout();
        Inspection result{size, framebuffer, {}};
        result.widgets.reserve(live_nodes.size() + (popup ? node(popup).choices.size() : 0));
        for (auto id : live_nodes) {
            auto& n = node(id);
            // Reparenting can move a group under a newer parent. Do not assume
            // that slot order or creation order is a topological ordering.
            n.inspection_enabled = n.enabled;
            for (auto parent = n.parent; id && parent; parent = node(parent).parent)
                n.inspection_enabled &= node(parent).enabled;
            n.inspection_enabled &= node(0).enabled;
            WidgetSnapshot snapshot;
            snapshot.id = n.identity;
            snapshot.parent = id ? node(n.parent).identity : 0;
            snapshot.role = role(n.kind);
            snapshot.label = n.kind == Kind::label || n.kind == Kind::button ? n.text : n.label;
            snapshot.text = n.kind == Kind::dropdown ? n.choices[n.selected] : n.text;
            snapshot.placeholder = n.placeholder;
            snapshot.in_layout = n.shown;
            snapshot.bounds = n.shown ? n.rect : Rect{};
            snapshot.clip = n.shown ? n.clip : Rect{};
            snapshot.visible = n.shown && n.clip.width > 0 && n.clip.height > 0;
            snapshot.enabled = n.inspection_enabled;
            snapshot.hovered = focused_window && n.hovered && n.shown;
            snapshot.focused = id && focus == id && n.shown && n.active;
            snapshot.pressed = id && n.shown && n.active &&
                               (capture == id || (focus == id && pressed_key != input::Key::unknown));
            snapshot.expanded = id && popup == id;
            snapshot.selected = n.kind == Kind::button && n.checked;
            if (scalar_control(n.kind)) {
                snapshot.number = n.value;
                snapshot.minimum = n.minimum;
                snapshot.maximum = n.maximum;
            } else if (n.kind == Kind::checkbox) {
                snapshot.checked = n.checked;
            }
            result.widgets.push_back(std::move(snapshot));
        }
        for (auto id : live_nodes) {
            const auto& n = node(id);
            if (n.kind != Kind::column || n.scrollbar == ScrollBar::hidden ||
                (!n.max_scroll && n.scrollbar != ScrollBar::always)) continue;
            WidgetSnapshot bar;
            bar.id = n.identity + 1;
            bar.parent = n.identity;
            bar.role = WidgetRole::scrollbar;
            bar.label = "Vertical scroll";
            bar.in_layout = n.shown;
            bar.bounds = n.shown ? n.scroll_track : Rect{};
            bar.clip = n.shown ? intersect(n.scroll_track, n.clip) : Rect{};
            bar.thumb = n.shown ? n.scroll_thumb : Rect{};
            bar.visible = n.shown && bar.clip.width > 0 && bar.clip.height > 0;
            bar.enabled = n.inspection_enabled && n.max_scroll > 0;
            bar.hovered = focused_window && bar.visible && bar.clip.contains(pointer);
            bar.pressed = n.active && n.shown && capture == id && capture_scrollbar;
            bar.number = n.scroll;
            bar.minimum = 0.F;
            bar.maximum = n.max_scroll;
            result.widgets.push_back(std::move(bar));
        }
        if (popup) {
            const auto& n = node(popup);
            const auto bounds = popup_rect();
            const auto clip = intersect(bounds, {0, 0, size.x, size.y});
            const auto count = std::min<std::size_t>(8, n.choices.size() - n.popup_first);
            for (std::size_t index = 0; index < n.choices.size(); ++index) {
                WidgetSnapshot option;
                option.id = n.identity + 2 + index;
                option.parent = n.identity;
                option.role = WidgetRole::option;
                option.label = option.text = n.choices[index];
                option.option_index = index;
                option.selected = index == n.selected;
                option.enabled = n.active;
                option.in_layout = index >= n.popup_first && index < n.popup_first + count;
                if (option.in_layout) {
                    option.bounds = {bounds.x, bounds.y + static_cast<f32>(index - n.popup_first) *
                                                            style().control_height,
                                     bounds.width, style().control_height};
                    option.clip = intersect(option.bounds, clip);
                    option.visible = option.clip.width > 0 && option.clip.height > 0;
                    option.hovered = focused_window && option.clip.contains(pointer);
                    option.pressed = capture == popup && option.hovered;
                }
                result.widgets.push_back(std::move(option));
            }
        }
        return result;
    }
};

std::shared_ptr<State> get(const Handle& h) {
    auto s = h.state.lock();
    if (!s || !s->live(h))
        throw std::logic_error("UI handle no longer refers to a live widget");
    return s;
}
bool valid(const Handle& h) noexcept {
    auto s = h.state.lock();
    return s && s->live(h);
}
bool flag(const Handle& h, Flag f) noexcept {
    auto s = h.state.lock();
    if (!s || !s->live(h))
        return false;
    const auto& n = s->node(h.id);
    if (f == Flag::clicked)
        return n.clicked;
    if (f == Flag::changed)
        return n.changed;
    if (f == Flag::submitted)
        return n.submitted;
    if (f == Flag::edit_started)
        return n.edit_started;
    if (f == Flag::edit_committed)
        return n.edit_committed;
    if (f == Flag::edit_cancelled)
        return n.edit_cancelled;
    // Ancestor flags are authoritative even before deferred layout runs.
    bool shown = true, active = true;
    for (auto id = h.id; id; id = s->node(id).parent) {
        shown &= s->node(id).visible;
        active &= s->node(id).enabled;
    }
    shown &= s->node(0).visible;
    active &= s->node(0).enabled;
    if (f == Flag::hovered)
        return s->focused_window && n.hovered && shown;
    if (f == Flag::focused)
        return h.id && s->focus == h.id && shown && active;
    return h.id && shown && active &&
           (s->capture == h.id || (s->focus == h.id && s->pressed_key != input::Key::unknown));
}
void number(const Handle& h, Property property, f32 v) {
    finite(v, 0, 100000);
    auto s = get(h);
    auto& n = s->node(h.id);
    f32* target{};
    switch (property) {
    case Property::width:
        target = &n.width;
        break;
    case Property::height:
        target = &n.height;
        break;
    case Property::padding:
        target = &n.padding;
        break;
    case Property::gap:
        target = &n.gap;
        break;
    case Property::scroll:
        target = &n.scroll;
        break;
    }
    if (!target || *target == v)
        return;
    *target = v;
    s->dirty = true;
}
void scrollbar(const Handle& h, ScrollBar mode) {
    auto s = get(h);
    auto& n = s->node(h.id);
    if (n.kind != Kind::column && mode != ScrollBar::hidden)
        throw std::invalid_argument("Vertical scrollbars require a column");
    if (mode != ScrollBar::automatic && mode != ScrollBar::always && mode != ScrollBar::hidden)
        throw std::invalid_argument("Unknown scrollbar mode");
    if (n.scrollbar == mode) return;
    n.scrollbar = mode;
    s->dirty = true;
    s->layout();
}
f32 scroll(const Handle& h, bool limit) {
    auto s = get(h);
    s->layout();
    return limit ? s->node(h.id).max_scroll : s->node(h.id).scroll;
}
void position(const Handle& h, Vec2 p) {
    finite(p.x, -100000, 100000);
    finite(p.y, -100000, 100000);
    auto s = get(h);
    if (s->node(h.id).position == p)
        return;
    s->node(h.id).position = p;
    s->dirty = true;
}
void visible(const Handle& h, bool b) {
    auto s = get(h);
    if (s->node(h.id).visible == b)
        return;
    s->node(h.id).visible = b;
    s->dirty = true;
    if (!b) s->invalidate_interaction(h.id);
}
void enabled(const Handle& h, bool b) {
    auto s = get(h);
    if (s->node(h.id).enabled == b)
        return;
    s->node(h.id).enabled = b;
    s->dirty = true;
    if (!b) s->invalidate_interaction(h.id);
}
void reparent(const Handle& child, const Handle& parent) {
    auto s = get(child);
    if (s != get(parent) || !child.id || !container(s->node(parent.id).kind))
        throw std::invalid_argument("Reparenting requires a non-root child and a container in the same Screen");
    std::size_t depth = 1;
    for (auto at = parent.id; at; at = s->node(at).parent) {
        if (at == child.id) throw std::invalid_argument("Reparenting cannot create a UI cycle");
        ++depth;
    }
    auto& n = s->node(child.id);
    auto& destination = s->node(parent.id).children;
    if (n.parent == parent.id && destination.back() == child.id) return;
    const auto check_depth = [&](auto&& self, u32 id, std::size_t level) -> void {
        if (level > 128) throw std::length_error("UI nesting limit exceeded");
        for (auto descendant : s->node(id).children) self(self, descendant, level + 1);
    };
    check_depth(check_depth, child.id, depth);
    destination.reserve(destination.size() + 1); // Preserve the tree if allocation fails.
    s->invalidate_interaction(child.id);
    std::erase(s->node(n.parent).children, child.id);
    destination.push_back(child.id);
    n.parent = parent.id;
    s->dirty = true;
}
void remove(const Handle& h) {
    auto s = get(h);
    if (!h.id)
        throw std::invalid_argument("Cannot remove UI root");
    // Allocate before touching focus/tree state. Subtree teardown then cannot
    // fail halfway through because the free-slot stack needs to grow.
    if (s->free_slots.capacity() < s->nodes.size())
        s->free_slots.reserve(std::max(s->nodes.size(), s->free_slots.capacity() * 2));
    s->invalidate_interaction(h.id);
    std::erase(s->node(s->node(h.id).parent).children, h.id);
    const auto destroy = [&](auto&& self, u32 id) -> void {
        for (auto c : s->node(id).children)
            self(self, c);
        s->live_nodes.erase(s->node(id).live_entry);
        s->nodes[id].reset();
        s->free_slots.push_back(id);
    };
    destroy(destroy, h.id);
    s->dirty = true;
}
void focus(const Handle& h) {
    auto s = get(h);
    s->layout();
    const auto& n = s->node(h.id);
    if (n.active && n.shown && interactive(n.kind))
        s->set_focus(h.id);
}
void reveal(const Handle& h) {
    auto s = get(h);
    s->layout();
    if (!s->node(h.id).shown) return;
    for (auto id = s->node(h.id).parent; id; id = s->node(id).parent) {
        auto& parent = s->node(id);
        if (parent.kind != Kind::column || parent.max_scroll <= 0) continue;
        const auto target = s->node(h.id).rect;
        const auto top = parent.rect.y + s->pad(parent);
        const auto bottom = top + parent.scroll_viewport;
        const auto delta = target.y < top || target.height > parent.scroll_viewport
            ? target.y - top : std::max(0.F, target.y + target.height - bottom);
        const auto next = std::clamp(parent.scroll + delta, 0.F, parent.max_scroll);
        if (next == parent.scroll) continue;
        parent.scroll = next;
        s->dirty = true;
        s->layout();
    }
}
Rect bounds(const Handle& h) {
    auto s = get(h);
    s->layout();
    return s->node(h.id).rect;
}
void string(const Handle& h, std::string_view text, bool placeholder) {
    auto s = get(h);
    auto& n = s->node(h.id);
    validate_text(text);
    if (placeholder) {
        n.placeholder = std::string(text);
        return;
    }
    if (!editable(n.kind) && n.text == text)
        return;
    n.text = editable(n.kind) ? normalize_text(text, n.kind == Kind::area) : std::string(text);
    n.edit = {};
    n.edit.caret = n.edit.anchor = n.text.size();
    n.changed = n.submitted = false;
    // Text never changes control height. Only a natural-width child in a row
    // (or the root) can change geometry; fixed/column-sized labels and numeric
    // fields must not relayout an entire inspector when their values change.
    if (n.width < 0 && s->node(n.parent).kind != Kind::column)
        s->dirty = true;
}
std::string_view string(const Handle& h) {
    return get(h)->node(h.id).text;
}
void checked(const Handle& h, bool v) {
    auto s = get(h);
    s->node(h.id).checked = v;
    s->node(h.id).changed = false;
}
bool checked(const Handle& h) {
    return get(h)->node(h.id).checked;
}
void selected(const Handle& h, std::size_t i) {
    auto s = get(h);
    auto& n = s->node(h.id);
    if (i >= n.choices.size())
        throw std::out_of_range("UI choice");
    n.selected = i;
    n.changed = false;
}
std::size_t selected(const Handle& h) {
    return get(h)->node(h.id).selected;
}
void range(const Handle& h, f32 minimum, f32 maximum) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum ||
        !std::isfinite(maximum - minimum))
        throw std::invalid_argument("Slider requires a finite increasing range");
    auto s = get(h);
    auto& n = s->node(h.id);
    // Synchronizing an unchanged range is not a new interaction. In particular,
    // do not cancel a captured drag or erase its same-frame change notification.
    if (n.minimum == minimum && n.maximum == maximum)
        return;
    s->cancel_scalar_edit(n);
    if (s->capture == h.id)
        s->capture = 0;
    n.minimum = minimum;
    n.maximum = maximum;
    n.value = std::clamp(n.value, minimum, maximum);
    n.changed = false;
}
void value(const Handle& h, f32 value) {
    if (!std::isfinite(value))
        throw std::invalid_argument("Slider value must be finite");
    auto s = get(h);
    auto& n = s->node(h.id);
    n.value = std::clamp(value, n.minimum, n.maximum);
    n.changed = false;
}
f32 value(const Handle& h) {
    return get(h)->node(h.id).value;
}
void step(const Handle& h, f32 value) {
    if (!std::isfinite(value) || value < 0)
        throw std::invalid_argument("Slider step must be finite and nonnegative");
    get(h)->node(h.id).step = value;
}
void split_axis(const Handle& h, SplitAxis axis) {
    auto s = get(h);
    s->node(h.id).split_axis = axis;
    s->dirty = true;
}
void image(const Handle& h, std::shared_ptr<const gfx::ImageData> pixels,
           std::optional<u64> revision) {
    if (pixels &&
        (pixels->extent.empty() || pixels->extent.width > 16384 || pixels->extent.height > 16384 ||
         pixels->pixels.size() !=
             static_cast<std::size_t>(pixels->extent.width) * pixels->extent.height * 4))
        throw std::invalid_argument("UI image needs tightly packed RGBA8 pixels");
    auto& n = get(h)->node(h.id);
    if (!n.image_identity)
        n.image_identity = std::make_shared<char>();
    n.pixels = std::move(pixels);
    n.image_revision = revision.value_or(n.image_revision + 1);
}
Handle append(const Handle& h, Kind kind, std::string_view label,
              std::vector<std::string> choices) {
    auto s = get(h);
    validate_text(label);
    if (!container(s->node(h.id).kind))
        throw std::invalid_argument("Only UI containers own children");
    if (s->live_nodes.size() >= max_widgets)
        throw std::length_error("UI widget limit exceeded");
    std::size_t depth{};
    for (auto id = h.id; id; id = s->node(id).parent)
        if (++depth >= 128)
            throw std::length_error("UI nesting limit exceeded");
    for (const auto& c : choices)
        validate_text(c);
    // One identity range per allocation: the widget, its possible scrollbar,
    // then its immutable dropdown choices. These IDs never alias reused slots.
    const auto identities = static_cast<u64>(choices.size()) + 2;
    if (identities > std::numeric_limits<u64>::max() - s->next_identity)
        throw std::length_error("UI widget identity space exhausted");
    auto n = std::make_unique<Node>();
    n->identity = s->next_identity;
    n->kind = kind;
    n->parent = h.id;
    if (kind == Kind::label || kind == Kind::button)
        n->text = label;
    else
        n->label = label;
    n->choices = std::move(choices);
    const bool reuse = !s->free_slots.empty();
    const auto id = reuse ? s->free_slots.back() : static_cast<u32>(s->nodes.size());
    s->node(h.id).children.push_back(id);
    try {
        s->live_nodes.push_back(id);
    } catch (...) {
        s->node(h.id).children.pop_back();
        throw;
    }
    n->live_entry = std::prev(s->live_nodes.end());
    if (reuse) {
        s->nodes[id] = std::move(n);
        s->free_slots.pop_back();
    } else {
        try {
            s->nodes.push_back(std::move(n));
        } catch (...) {
            s->live_nodes.pop_back();
            s->node(h.id).children.pop_back();
            throw;
        }
    }
    s->next_identity += identities;
    s->dirty = true;
    return {s, id, s->node(id).identity};
}
} // namespace vng::ui::detail

namespace vng::ui {
Screen::Screen(std::shared_ptr<const Theme> theme) : state_(std::make_shared<detail::State>()) {
    detail::validate_theme(theme);
    state_->theme = std::move(theme);
    state_->nodes.push_back(std::make_unique<detail::Node>());
    state_->live_nodes.push_back(0);
    state_->nodes[0]->live_entry = state_->live_nodes.begin();
    state_->nodes[0]->identity = state_->next_identity;
    state_->next_identity += 2;
    state_->nodes[0]->kind = detail::Kind::root;
}
Container Screen::root() {
    return Container{{state_, 0, state_ ? state_->node(0).identity : 0}};
}
void Screen::set_theme(std::shared_ptr<const Theme> theme) {
    detail::validate_theme(theme);
    if (!state_)
        throw std::logic_error("Empty UI screen");
    state_->theme = std::move(theme);
    state_->dirty = true;
    for (auto id : state_->live_nodes)
        state_->node(id).edit.dirty = true;
}
Result<UpdateResult> Screen::update(const input::Frame& frame, f32 seconds,
                                    input::Clipboard* clipboard) {
    try {
        if (!state_)
            throw std::logic_error("Empty UI screen");
        detail::finite(seconds, 0, 3600);
        detail::finite(frame.logical_size.x, 1, 100000);
        detail::finite(frame.logical_size.y, 1, 100000);
        detail::finite(frame.pointer.x, -1e7F, 1e7F);
        detail::finite(frame.pointer.y, -1e7F, 1e7F);
        if (frame.events.size() > 65536)
            throw std::invalid_argument("UI input event limit exceeded");
        for (const auto& event : frame.events) {
            detail::finite(event.position.x, -1e7F, 1e7F);
            detail::finite(event.position.y, -1e7F, 1e7F);
            detail::finite(event.scroll.x, -1e7F, 1e7F);
            detail::finite(event.scroll.y, -1e7F, 1e7F);
            if (event.kind == input::EventKind::text)
                detail::validate_text(event.text);
        }
        if (frame.framebuffer.empty())
            throw std::invalid_argument("UI requires a nonempty framebuffer");
        auto& s = *state_;
        const Vec2 scale{static_cast<f32>(frame.framebuffer.width) / frame.logical_size.x,
                         static_cast<f32>(frame.framebuffer.height) / frame.logical_size.y};
        detail::finite(scale.x, 0, 64);
        detail::finite(scale.y, 0, 64);
        if (s.size != frame.logical_size || s.scale != scale) {
            s.size = frame.logical_size;
            s.scale = scale;
            s.dirty = true;
            for (auto id : s.live_nodes)
                s.node(id).edit.dirty = true;
        }
        s.framebuffer = frame.framebuffer;
        s.clock = std::fmod(s.clock + seconds, 1.0F);
        for (auto id : s.live_nodes) {
            auto& n=s.node(id);
            n.clicked = n.changed = n.submitted = n.hovered = false;
            n.edit_started = n.edit_committed = n.edit_cancelled = false;
        }
        s.layout();
        s.pointer = frame.pointer;
        const auto has_focus_event = std::ranges::any_of(frame.events, [](const input::Event& e) {
            return e.kind == input::EventKind::focus_lost ||
                   e.kind == input::EventKind::focus_gained;
        });
        // The snapshot is the state at the end of the queue. Do not use it to
        // discard committed text that arrived before a queued focus loss.
        if (!has_focus_event)
            s.focused_window = frame.focused;
        if (frame.overflow || (!frame.focused && !has_focus_event)) {
            s.cancel_capture();
            s.set_focus(0);
            s.capture = s.popup = 0;
            s.release_owned = false;
            s.owned_keys.fill(false);
        }
        UpdateResult result;
        if (frame.overflow)
            throw std::runtime_error("Window input queue overflowed; UI focus/capture cancelled");
        for (std::size_t index = 0; index < frame.events.size(); ++index) {
            const auto& event = frame.events[index];
            // A captured scalar/scrollbar drag depends only on the latest
            // pointer position, not its path. Fold contiguous motion samples
            // before doing hit tests or (for scrollbars) relaying out the tree.
            // Never cross a release, key, focus or other event; uncaptured and
            // application-owned motion remains unchanged in unhandled().
            if (event.kind == input::EventKind::pointer_move && s.capture &&
                (s.capture_scrollbar || detail::scalar_control(s.node(s.capture).kind)) &&
                index + 1 < frame.events.size() &&
                frame.events[index + 1].kind == input::EventKind::pointer_move)
                continue;
            const bool consumed = s.event(event, clipboard);
            if (consumed && event.kind == input::EventKind::key_down) {
                const auto i = static_cast<std::size_t>(event.key);
                if (i < s.owned_keys.size())
                    s.owned_keys[i] = true;
            }
            if (!consumed)
                result.events.push_back(event);
        }
        if (!frame.focused) {
            s.cancel_capture();
            s.focused_window = false;
            s.set_focus(0);
            s.capture = s.popup = 0;
            s.release_owned = false;
            s.owned_keys.fill(false);
        }
        if (s.focused_window)
            for (auto id = s.hit(s.pointer); id; id = s.node(id).parent)
                s.node(id).hovered = true;
        result.capturesKeyboard = s.focus || s.popup || s.capture_scrollbar;
        result.capturesShortcuts = s.popup || s.capture || s.capture_scrollbar ||
            (s.focus && (s.node(s.focus).kind == detail::Kind::input ||
                         s.node(s.focus).kind == detail::Kind::area));
        const auto pointer_target = s.hit(s.pointer);
        result.capturesPointer =
            s.focused_window &&
            (s.capture || s.popup ||
             (pointer_target && s.node(pointer_target).kind != detail::Kind::image));
        result.keys = frame.keys;
        for (std::size_t i = 0; i < result.keys.size(); ++i)
            if (result.capturesKeyboard || s.owned_keys[i] || !frame.focused)
                result.keys[i] = false;
        return result;
    } catch (const std::exception& e) {
        return std::unexpected(Diagnostic{e.what()});
    }
}
Result<DrawList> Screen::draw_list() const {
    try {
        if (!state_ || state_->framebuffer.empty())
            throw std::logic_error("Update UI before rendering it");
        return state_->paint();
    } catch (const std::exception& e) {
        return std::unexpected(Diagnostic{e.what()});
    }
}
Result<Inspection> Screen::inspect() const {
    try {
        if (!state_ || state_->framebuffer.empty())
            throw std::logic_error("Update UI before inspecting it");
        return state_->inspect();
    } catch (const std::exception& e) {
        return std::unexpected(Diagnostic{e.what()});
    }
}
} // namespace vng::ui
