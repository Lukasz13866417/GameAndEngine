#pragma once
#include <vng/ui/ui.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <unicode/ubrk.h>
#include <unicode/utext.h>
#include <unicode/ustring.h>

namespace vng::ui::detail {
inline void validate_text(std::string_view text) {
    if (text.size() > 65536)
        throw std::invalid_argument("UI text exceeds 64 KiB");
    UErrorCode error = U_ZERO_ERROR;
    int32_t length{};
    u_strFromUTF8(nullptr, 0, &length, text.data(), static_cast<int32_t>(text.size()), &error);
    if (error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(error))
        throw std::invalid_argument("UI text must be valid UTF-8");
}
inline std::string normalize_text(std::string_view text, bool multiline) {
    validate_text(text);
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            out += multiline ? '\n' : ' ';
        } else if (c == '\n')
            out += multiline ? '\n' : ' ';
        else if (c == '\t')
            out += "    ";
        else if (static_cast<unsigned char>(c) >= 32 && c != 127)
            out += c;
    }
    validate_text(out);
    return out;
}
inline std::vector<std::size_t> boundaries(std::string_view text) {
    UErrorCode error = U_ZERO_ERROR;
    auto* iterator = ubrk_open(UBRK_CHARACTER, "root", nullptr, 0, &error);
    if (U_FAILURE(error))
        throw std::runtime_error("Cannot create Unicode grapheme iterator");
    struct Close {
        UBreakIterator* p;
        ~Close() { ubrk_close(p); }
    } close{iterator};
    UText storage = UTEXT_INITIALIZER;
    utext_openUTF8(&storage, text.data(), static_cast<int64_t>(text.size()), &error);
    struct TextClose {
        UText* p;
        ~TextClose() { utext_close(p); }
    } text_close{&storage};
    ubrk_setUText(iterator, &storage, &error);
    if (U_FAILURE(error))
        throw std::runtime_error("Cannot segment UI text");
    std::vector<std::size_t> result;
    for (auto p = ubrk_first(iterator); p != UBRK_DONE; p = ubrk_next(iterator))
        result.push_back(static_cast<std::size_t>(p));
    return result;
}
struct Caret {
    std::size_t byte{};
    Vec2 position{};
};
struct TextGeometry {
    std::vector<Caret> carets;
    f32 line_height{1}, width{}, height{};
};
inline TextGeometry geometry(std::string_view text, const ThemeValues& style, Vec2 scale) {
    TextGeometry out;
    const auto size =
        static_cast<u32>(std::clamp(std::round(style.font_size * scale.y), 1.0F, 4096.0F));
    std::size_t begin{};
    f32 y{};
    while (true) {
        auto end = text.find('\n', begin);
        if (end == std::string_view::npos)
            end = text.size();
        const auto line = text.substr(begin, end - begin);
        auto shaped = style.font.layout(line, size);
        if (!shaped)
            throw std::runtime_error(shaped.error().message);
        out.line_height = shaped->metrics.line_height / scale.y;
        const auto stops = boundaries(line);
        struct Cluster {
            f32 lo{1e20F}, hi{-1e20F};
            bool rtl{};
        };
        std::map<std::size_t, Cluster> clusters;
        for (const auto& glyph : shaped->glyphs) {
            auto& c = clusters[glyph.cluster];
            c.lo = std::min(c.lo, glyph.pen.x);
            c.hi = std::max(c.hi, glyph.pen.x + glyph.advance.x);
            c.rtl = glyph.right_to_left;
        }
        for (const auto stop : stops) {
            f32 x{};
            auto next = clusters.upper_bound(stop);
            if (next != clusters.begin()) {
                const auto current = std::prev(next);
                const auto limit = next == clusters.end() ? line.size() : next->first;
                const auto first_stop =
                    std::lower_bound(stops.begin(), stops.end(), current->first);
                const auto final_stop = std::lower_bound(stops.begin(), stops.end(), limit);
                const auto this_stop = std::lower_bound(stops.begin(), stops.end(), stop);
                const auto count = std::max<std::ptrdiff_t>(1, final_stop - first_stop);
                const auto t = std::clamp(
                    static_cast<f32>(this_stop - first_stop) / static_cast<f32>(count), 0.0F, 1.0F);
                const auto& c = current->second;
                x = c.rtl ? c.hi - (c.hi - c.lo) * t : c.lo + (c.hi - c.lo) * t;
            }
            out.carets.push_back({begin + stop, {x / scale.x, y}});
        }
        out.width = std::max(out.width, shaped->metrics.width / scale.x);
        y += out.line_height;
        if (end == text.size())
            break;
        begin = end + 1;
    }
    out.height = y;
    return out;
}
struct EditSnapshot {
    std::string text;
    std::size_t caret{}, anchor{};
};
struct Editor {
    std::size_t caret{}, anchor{};
    f32 scroll_x{}, scroll_y{};
    TextGeometry geometry;
    bool dirty{true};
    std::vector<EditSnapshot> undo, redo;
    std::size_t previous() const {
        for (std::size_t i = geometry.carets.size(); i-- > 0;)
            if (geometry.carets[i].byte < caret)
                return geometry.carets[i].byte;
        return 0;
    }
    std::size_t next(std::size_t size) const {
        for (const auto& c : geometry.carets)
            if (c.byte > caret)
                return c.byte;
        return size;
    }
    Vec2 at(std::size_t byte) const {
        for (const auto& c : geometry.carets)
            if (c.byte >= byte)
                return c.position;
        return geometry.carets.empty() ? Vec2{} : geometry.carets.back().position;
    }
    std::size_t nearest(Vec2 p) const {
        f32 best = 1e30F;
        std::size_t result{};
        const auto line =
            std::max(0.0F, std::floor(p.y / geometry.line_height)) * geometry.line_height;
        for (const auto& c : geometry.carets) {
            const auto score =
                std::abs(c.position.y - line) * 100000 + std::abs(c.position.x - p.x);
            if (score < best) {
                best = score;
                result = c.byte;
            }
        }
        return result;
    }
    void remember(const std::string& text) {
        if (undo.size() == 128)
            undo.erase(undo.begin());
        undo.push_back({text, caret, anchor});
        redo.clear();
    }
};
} // namespace vng::ui::detail
