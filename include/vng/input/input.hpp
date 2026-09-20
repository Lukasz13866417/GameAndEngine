#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <vng/core/types.hpp>

namespace vng::input {
enum class Key {
    unknown,
    escape,
    space,
    enter,
    tab,
    backspace,
    del,
    left,
    right,
    up,
    down,
    home,
    end,
    page_up,
    page_down,
    a,
    b,
    c,
    d,
    e,
    f,
    g,
    h,
    i,
    j,
    k,
    l,
    m,
    n,
    o,
    p,
    q,
    r,
    s,
    t,
    u,
    v,
    w,
    x,
    y,
    z,
    f11,
    one, two, three, four, five,
    count
};
enum class EventKind {
    pointer_move,
    pointer_down,
    pointer_up,
    scroll,
    key_down,
    key_up,
    text,
    focus_lost,
    focus_gained
};
struct Modifiers {
    bool shift{}, control{}, alt{}, super{};
    bool left_alt{}; // Side-specific navigation modifier; Right Alt/AltGr is distinct.
};
struct Event {
    EventKind kind{};
    Vec2 position{}; // logical window coordinates, top-left origin
    Vec2 scroll{};
    Key key{Key::unknown};
    u32 button{}; // 0 left, 1 right, 2 middle
    Modifiers modifiers{};
    bool repeat{};
    std::string text{}; // committed UTF-8, never derived from a key code
    // CPU callback receipt, not a device timestamp. Zero means untimed/synthetic input.
    u64 received_ns{};
};
struct Frame {
    Vec2 logical_size{};
    Extent2D framebuffer{};
    Vec2 pointer{};
    bool focused{true};
    bool overflow{};
    std::array<bool, static_cast<std::size_t>(Key::count)> keys{};
    std::vector<Event> events{};
    [[nodiscard]] bool keyDown(Key key) const noexcept {
        const auto i = static_cast<std::size_t>(key);
        return i < keys.size() && keys[i];
    }
};
// Optional host services. UI tests/headless tools can supply an in-memory one.
class Clipboard {
public:
    virtual ~Clipboard() = default;
    virtual std::string read() = 0;
    virtual void write(std::string_view) = 0;
};
} // namespace vng::input
