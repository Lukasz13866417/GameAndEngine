#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vng/core/types.hpp>

namespace vng::text {

enum class ErrorCode {
    invalid_font,
    file_read_failed,
    invalid_size,
    invalid_utf8,
    text_too_large,
    shaping_failed,
    rasterization_failed,
};

struct Diagnostic final {
    ErrorCode code{ErrorCode::invalid_font};
    std::string message{};
};

struct TextMetrics final {
    // Advance width and line-box height, not the bounds of the visible ink.
    f32 width{};
    f32 height{};
    f32 baseline{};
    f32 line_height{};
    u32 line_count{};
};

struct PositionedGlyph final {
    u32 glyph_index{};
    // Baseline origin relative to the top-left line box, with +Y down.
    Vec2 position{};
    // Byte offset in the original UTF-8 string, including newline/tab bytes.
    u32 cluster{};
    // Shaping pen/advance, separate from ink offsets, for caret placement.
    Vec2 pen{};
    Vec2 advance{};
    bool right_to_left{};
};

struct TextLayout final {
    std::vector<PositionedGlyph> glyphs{};
    TextMetrics metrics{};
};

struct GlyphBitmap final {
    u32 width{};
    u32 height{};
    i32 left{};
    i32 top{};
    // Tightly packed 8-bit coverage, top row first. Bitmap position is
    // glyph.position + Vec2{left, -top}, plus the requested text position.
    std::vector<std::uint8_t> pixels{};
};

namespace detail {
struct FontData;
}

// A cheap shared owning handle. Font file bytes remain owned after load(), and
// layout/rasterize serialize access to the underlying face across handle copies.
class Font final {
public:
    Font() = default;

    [[nodiscard]] static std::expected<Font, Diagnostic>
    load(const std::filesystem::path& path);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] std::uint64_t id() const noexcept;

    // Size is in framebuffer pixels, in [1, 4096]. Each tab-separated segment
    // is shaped as one run with inferred script/direction; full paragraph bidi,
    // font fallback, wrapping, and color emoji are not performed. Newlines and
    // CRLF are supported; tabs advance to the next four-space stop.
    [[nodiscard]] std::expected<TextLayout, Diagnostic>
    layout(std::string_view utf8, u32 pixel_size) const;

    [[nodiscard]] std::expected<TextMetrics, Diagnostic>
    measure(std::string_view utf8, u32 pixel_size) const;

    [[nodiscard]] std::expected<GlyphBitmap, Diagnostic>
    rasterize(u32 glyph_index, u32 pixel_size) const;

private:
    explicit Font(std::shared_ptr<detail::FontData> data) : data_(std::move(data)) {}
    std::shared_ptr<detail::FontData> data_{};
};

} // namespace vng::text
