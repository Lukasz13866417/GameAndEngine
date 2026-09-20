#pragma once
#include <concepts>
#include <cmath>
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vng/input/input.hpp>
#include <vng/ui/draw_list.hpp>
#include <vng/ui/inspection.hpp>

namespace vng::ui {
// Live widgets per screen; storage grows on demand and removed slots are reused.
inline constexpr std::size_t max_widgets = 262144;
struct Diagnostic {
    std::string message;
};
template <class T> using Result = std::expected<T, Diagnostic>;
// Scrollbars navigate overflowing columns; they are not value sliders.
enum class ScrollBar { automatic, always, hidden };
// Direction the divider moves: y separates stacked panes; x separates columns.
enum class SplitAxis { x, y };
namespace detail {
struct State;
enum class Kind { root, column, row, label, button, checkbox, dropdown, input, area, slider, image, splitter };
enum class Property { width, height, padding, gap, scroll };
enum class Flag { hovered, pressed, focused, clicked, changed, submitted, edit_started, edit_committed, edit_cancelled };
struct Handle {
    std::weak_ptr<State> state;
    u32 id{};
    u64 identity{}; // allocation generation; a recycled slot is a different widget
};
bool valid(const Handle&) noexcept;
bool flag(const Handle&, Flag) noexcept;
void number(const Handle&, Property, f32);
void scrollbar(const Handle&, ScrollBar);
f32 scroll(const Handle&, bool limit = false);
void position(const Handle&, Vec2);
void visible(const Handle&, bool);
void enabled(const Handle&, bool);
void remove(const Handle&);
void reparent(const Handle& child, const Handle& parent);
void focus(const Handle&);
void reveal(const Handle&);
Rect bounds(const Handle&);
void string(const Handle&, std::string_view, bool placeholder = false);
std::string_view string(const Handle&);
void checked(const Handle&, bool);
bool checked(const Handle&);
void selected(const Handle&, std::size_t);
std::size_t selected(const Handle&);
void range(const Handle&, f32 minimum, f32 maximum);
void value(const Handle&, f32);
f32 value(const Handle&);
void step(const Handle&, f32);
void split_axis(const Handle&, SplitAxis);
void image(const Handle&, std::shared_ptr<const gfx::ImageData>, std::optional<u64>);
Handle append(const Handle&, Kind, std::string_view, std::vector<std::string> choices = {});
} // namespace detail

template <class Derived> class WidgetHandle {
public:
    WidgetHandle() = default;
    explicit WidgetHandle(detail::Handle h) : handle_(std::move(h)) {}
    [[nodiscard]] bool valid() const noexcept { return detail::valid(handle_); }
    [[nodiscard]] bool isHovered() const noexcept {
        return detail::flag(handle_, detail::Flag::hovered);
    }
    [[nodiscard]] bool isPressed() const noexcept {
        return detail::flag(handle_, detail::Flag::pressed);
    }
    [[nodiscard]] bool isFocused() const noexcept {
        return detail::flag(handle_, detail::Flag::focused);
    }
    [[nodiscard]] Rect bounds() const { return detail::bounds(handle_); }
    Derived& position(Vec2 p) {
        detail::position(handle_, p);
        return self();
    }
    Derived& width(f32 n) {
        detail::number(handle_, detail::Property::width, n);
        return self();
    }
    Derived& height(f32 n) {
        detail::number(handle_, detail::Property::height, n);
        return self();
    }
    Derived& visible(bool b) {
        detail::visible(handle_, b);
        return self();
    }
    Derived& enabled(bool b) {
        detail::enabled(handle_, b);
        return self();
    }
    void focus() { detail::focus(handle_); }
    // Scroll enclosing panels just enough to make this widget visible.
    void reveal() { detail::reveal(handle_); }
    void remove() { detail::remove(handle_); }

protected:
    Derived& self() { return static_cast<Derived&>(*this); }
    detail::Handle handle_;
};
class Label : public WidgetHandle<Label> {
public:
    using WidgetHandle::WidgetHandle;
    // Borrowed until the next text change, removal, or screen destruction.
    [[nodiscard]] std::string_view text() const { return detail::string(handle_); }
    Label& text(std::string_view s) {
        detail::string(handle_, s);
        return *this;
    }
};
class Button : public WidgetHandle<Button> {
public:
    using WidgetHandle::WidgetHandle;
    // Persistent emphasis for tabs/tools; clicking still reports clicked(),
    // leaving selection ownership with the application.
    Button& selected(bool value) {
        detail::checked(handle_, value);
        return *this;
    }
    [[nodiscard]] bool clicked() const noexcept {
        return detail::flag(handle_, detail::Flag::clicked);
    }
    Button& text(std::string_view s) {
        detail::string(handle_, s);
        return *this;
    }
};
class Checkbox : public WidgetHandle<Checkbox> {
public:
    using WidgetHandle::WidgetHandle;
    Checkbox& value(bool b) {
        detail::checked(handle_, b);
        return *this;
    }
    [[nodiscard]] bool value() const { return detail::checked(handle_); }
    [[nodiscard]] std::optional<bool> changedValue() const {
        return detail::flag(handle_, detail::Flag::changed) ? std::optional{value()} : std::nullopt;
    }
};
class TextField : public WidgetHandle<TextField> {
public:
    using WidgetHandle::WidgetHandle;
    TextField& value(std::string_view s) {
        detail::string(handle_, s);
        return *this;
    }
    TextField& placeholder(std::string_view s) {
        detail::string(handle_, s, true);
        return *this;
    }
    // Borrowed until the next edit, removal, or screen destruction.
    [[nodiscard]] std::string_view getText() const { return detail::string(handle_); }
    [[nodiscard]] std::optional<std::string_view> changedText() const {
        return detail::flag(handle_, detail::Flag::changed) ? std::optional{getText()}
                                                            : std::nullopt;
    }
    [[nodiscard]] std::optional<std::string_view> submittedText() const {
        return detail::flag(handle_, detail::Flag::submitted) ? std::optional{getText()}
                                                              : std::nullopt;
    }
    // Escape released this field's focus. The caller decides whether to restore
    // a draft; the text itself is retained, just like losing focus normally.
    [[nodiscard]] bool editCancelled() const noexcept {
        return detail::flag(handle_, detail::Flag::edit_cancelled);
    }
};
class Slider : public WidgetHandle<Slider> {
public:
    using WidgetHandle::WidgetHandle;
    Slider& value(f32 v) { detail::value(handle_, v); return *this; }
    [[nodiscard]] f32 value() const { return detail::value(handle_); }
    Slider& range(f32 minimum, f32 maximum) {
        detail::range(handle_, minimum, maximum); return *this;
    }
    // Zero chooses 1% of the range. Positive values snap pointer and keyboard edits.
    Slider& step(f32 increment) { detail::step(handle_, increment); return *this; }
    [[nodiscard]] std::optional<f32> changedValue() const {
        return detail::flag(handle_, detail::Flag::changed) ? std::optional{value()} : std::nullopt;
    }
    [[nodiscard]] bool editStarted() const noexcept { return detail::flag(handle_, detail::Flag::edit_started); }
    [[nodiscard]] bool editCommitted() const noexcept { return detail::flag(handle_, detail::Flag::edit_committed); }
    [[nodiscard]] bool editCancelled() const noexcept { return detail::flag(handle_, detail::Flag::edit_cancelled); }
};
// A captured, relative drag in logical pixels. The owner decides which panes
// to resize; no document, layout tree or backend is retained by the control.
class Splitter : public WidgetHandle<Splitter> {
public:
    using WidgetHandle::WidgetHandle;
    Splitter& value(f32 v) { detail::value(handle_, v); return *this; }
    [[nodiscard]] f32 value() const { return detail::value(handle_); }
    Splitter& range(f32 minimum, f32 maximum) {
        detail::range(handle_, minimum, maximum); return *this;
    }
    [[nodiscard]] std::optional<f32> changedValue() const {
        return detail::flag(handle_, detail::Flag::changed) ? std::optional{value()} : std::nullopt;
    }
    [[nodiscard]] bool editStarted() const noexcept { return detail::flag(handle_, detail::Flag::edit_started); }
    [[nodiscard]] bool editCommitted() const noexcept { return detail::flag(handle_, detail::Flag::edit_committed); }
    [[nodiscard]] bool editCancelled() const noexcept { return detail::flag(handle_, detail::Flag::edit_cancelled); }
};
class ImageView : public WidgetHandle<ImageView> {
public:
    using WidgetHandle::WidgetHandle;
    ImageView& image(std::shared_ptr<const gfx::ImageData> pixels) {
        detail::image(handle_, std::move(pixels), std::nullopt); return *this;
    }
    ImageView& image(std::shared_ptr<const gfx::ImageData> pixels, u64 revision) {
        detail::image(handle_, std::move(pixels), revision); return *this;
    }
    // Empty images draw nothing. Pointer events stay unhandled for viewport tools.
};
template <class T> struct Choice {
    T value;
    std::string label;
};
template <std::equality_comparable T> class Dropdown : public WidgetHandle<Dropdown<T>> {
public:
    Dropdown(detail::Handle handle, std::shared_ptr<const std::vector<T>> values)
        : WidgetHandle<Dropdown<T>>(std::move(handle)), values_(std::move(values)) {}
    [[nodiscard]] T value() const { return values_->at(detail::selected(this->handle_)); }
    Dropdown& value(const T& v) {
        for (std::size_t i = 0; i < values_->size(); ++i)
            if ((*values_)[i] == v) {
                detail::selected(this->handle_, i);
                return *this;
            }
        throw std::invalid_argument("Dropdown value is not one of its choices");
    }
    [[nodiscard]] std::optional<T> changedValue() const {
        return detail::flag(this->handle_, detail::Flag::changed) ? std::optional{value()}
                                                                  : std::nullopt;
    }

private:
    std::shared_ptr<const std::vector<T>> values_;
};
class Container : public WidgetHandle<Container> {
public:
    using WidgetHandle::WidgetHandle;
    // Move an existing group to the end of this container. Handles, values and
    // widget identities survive; both containers must belong to one Screen.
    // Moving cancels the group's active gesture/focus. Cycles are rejected.
    Container& adopt(Container child) {
        detail::reparent(child.handle_, handle_);
        return *this;
    }
    Container& padding(f32 n) {
        detail::number(handle_, detail::Property::padding, n);
        return *this;
    }
    Container& gap(f32 n) {
        detail::number(handle_, detail::Property::gap, n);
        return *this;
    }
    Container& scroll(f32 y) {
        detail::number(handle_, detail::Property::scroll, y);
        return *this;
    }
    [[nodiscard]] f32 scroll() const { return detail::scroll(handle_); }
    [[nodiscard]] f32 scroll_limit() const { return detail::scroll(handle_, true); }
    Container& scrollbar(ScrollBar mode) {
        detail::scrollbar(handle_, mode);
        return *this;
    }
    Container column() { return Container{detail::append(handle_, detail::Kind::column, {})}; }
    Container row() { return Container{detail::append(handle_, detail::Kind::row, {})}; }
    Label label(std::string_view s) {
        return Label{detail::append(handle_, detail::Kind::label, s)};
    }
    Button button(std::string_view s) {
        return Button{detail::append(handle_, detail::Kind::button, s)};
    }
    Checkbox checkbox(std::string_view s) {
        return Checkbox{detail::append(handle_, detail::Kind::checkbox, s)};
    }
    TextField text_input(std::string_view label = {}) {
        return TextField{detail::append(handle_, detail::Kind::input, label)};
    }
    TextField text_area(std::string_view label = {}) {
        return TextField{detail::append(handle_, detail::Kind::area, label)};
    }
    Slider slider(std::string_view label, f32 minimum, f32 maximum) {
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum ||
            !std::isfinite(maximum - minimum))
            throw std::invalid_argument("Slider requires a finite increasing range");
        Slider result{detail::append(handle_, detail::Kind::slider, label)};
        result.range(minimum, maximum);
        return result;
    }
    Splitter splitter(std::string_view label, SplitAxis axis = SplitAxis::y) {
        auto handle = detail::append(handle_, detail::Kind::splitter, label);
        detail::split_axis(handle, axis);
        return Splitter{std::move(handle)};
    }
    ImageView image() { return ImageView{detail::append(handle_, detail::Kind::image, {})}; }
    template <std::equality_comparable T>
    Dropdown<T> dropdown(std::string_view label, std::initializer_list<Choice<T>> choices) {
        return dropdown<T>(label, std::span<const Choice<T>>{choices.begin(), choices.size()});
    }
    template <std::equality_comparable T>
    Dropdown<T> dropdown(std::string_view label, std::span<const Choice<T>> choices) {
        if (choices.size() == 0)
            throw std::invalid_argument("Dropdown needs choices");
        auto values = std::make_shared<std::vector<T>>();
        std::vector<std::string> labels;
        for (const auto& c : choices) {
            for (const auto& v : *values)
                if (v == c.value)
                    throw std::invalid_argument("Duplicate dropdown value");
            values->push_back(c.value);
            labels.push_back(c.label);
        }
        return Dropdown<T>{
            detail::append(handle_, detail::Kind::dropdown, label, std::move(labels)),
            std::move(values)};
    }
};

struct UpdateResult {
    std::vector<input::Event> events;
    std::array<bool, static_cast<std::size_t>(input::Key::count)> keys{};
    bool capturesKeyboard{}, capturesPointer{};
    // Text editing, an open popup or an active pointer capture needs exclusive
    // shortcut handling. Mere button/dropdown focus still permits app shortcuts.
    bool capturesShortcuts{};
    [[nodiscard]] std::span<const input::Event> unhandled() const noexcept { return events; }
    [[nodiscard]] bool keyDown(input::Key k) const noexcept {
        const auto i = static_cast<std::size_t>(k);
        return i < keys.size() && keys[i];
    }
};
class Screen {
public:
    explicit Screen(std::shared_ptr<const Theme>);
    Screen(Screen&&) noexcept = default;
    Screen& operator=(Screen&&) noexcept = default;
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
    Container root();
    Container column() { return root().column(); }
    Container row() { return root().row(); }
    void set_theme(std::shared_ptr<const Theme>);
    [[nodiscard]] Result<UpdateResult> update(const input::Frame&, f32 seconds,
                                              input::Clipboard* = nullptr);
    // May refresh layout after setters, but never resets interaction events.
    [[nodiscard]] Result<DrawList> draw_list() const;
    // Read-only semantic/layout snapshot for tools and diagnostics. Like
    // draw_list(), refreshes pending layout and requires one successful update;
    // it neither synthesizes input nor consumes/resets interaction events.
    [[nodiscard]] Result<Inspection> inspect() const;

private:
    std::shared_ptr<detail::State> state_;
};
// Optional window bridge: the core depends on input/font types, never GLFW.
template <class Window>
[[nodiscard]] Result<UpdateResult> update(Screen& screen, Window& window, f32 seconds) {
    struct Clipboard final : input::Clipboard {
        Window& window;
        explicit Clipboard(Window& w) : window(w) {}
        std::string read() override { return window.clipboard_text(); }
        void write(std::string_view s) override { window.set_clipboard_text(s); }
    } clipboard{window};
    return screen.update(window.take_input(), seconds, &clipboard);
}
} // namespace vng::ui
