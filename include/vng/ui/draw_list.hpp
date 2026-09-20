#pragma once
#include <array>
#include <string>
#include <variant>
#include <vector>
#include <vng/gfx/image.hpp>
#include <vng/ui/theme.hpp>

namespace vng::ui {
struct Rect {
    f32 x{}, y{}, width{}, height{};
    [[nodiscard]] bool contains(Vec2 p) const noexcept {
        return width > 0 && height > 0 && p.x >= x && p.y >= y && p.x < x + width &&
               p.y < y + height;
    }
};
struct BoxDraw {
    Rect rect, clip;
    Vec4 color{}, border{};
    f32 radius{}, border_width{};
};
struct TextDraw {
    std::string text; // owned: a draw list remains safe after its screen changes
    Vec2 position{};
    Rect clip;
    text::Font font;
    f32 size{};
    Vec4 color{};
};
// Solid geometry for editor overlays; clipped in logical UI coordinates.
struct TriangleDraw {
    std::array<Vec2, 3> points;
    Rect clip;
    Vec4 color;
};
// Pixels are immutable snapshots: RGBA8, sRGB RGB, straight alpha, top row first.
// Identity is an owning token, not a GPU object. Keeping it stable lets a renderer
// reuse storage as a viewport receives new revisions. An empty token uses pixels
// as identity, which is convenient for manually constructed draw lists.
struct ImageDraw {
    Rect rect, clip;
    std::shared_ptr<const gfx::ImageData> pixels;
    std::shared_ptr<const void> identity;
    u64 revision{};
};
using DrawCommand = std::variant<BoxDraw, TextDraw, ImageDraw, TriangleDraw>;
struct DrawList {
    Vec2 logical_size{};
    Extent2D framebuffer{};
    std::vector<DrawCommand> commands;
};
} // namespace vng::ui
