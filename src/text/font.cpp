#include <vng/text/font.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <utility>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

namespace vng::text {
namespace detail {

struct FontData final {
    std::vector<unsigned char> bytes{};
    FT_Library library{};
    FT_Face face{};
    hb_font_t* shaping_font{};
    std::uint64_t identity{};
    u32 current_size{};
    std::mutex mutex{};

    ~FontData()
    {
        if (shaping_font) {
            hb_font_destroy(shaping_font);
        }
        if (face) {
            FT_Done_Face(face);
        }
        if (library) {
            FT_Done_FreeType(library);
        }
    }
};

} // namespace detail
namespace {

constexpr std::size_t maximum_font_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_text_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_bitmap_bytes = 64U * 1024U * 1024U;
constexpr u32 maximum_pixel_size = 4096;
std::atomic<std::uint64_t> next_identity{1};

[[nodiscard]] Diagnostic freetype_error(ErrorCode code, const char* operation, FT_Error error)
{
    return {code, std::string(operation) + " failed (FreeType error "
        + std::to_string(error) + ")"};
}

[[nodiscard]] bool valid_utf8(std::string_view text)
{
    for (std::size_t offset = 0; offset < text.size();) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        if (lead < 0x80) {
            ++offset;
            continue;
        }
        std::size_t length{};
        u32 codepoint{};
        u32 minimum{};
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            codepoint = lead & 0x1FU;
            minimum = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            codepoint = lead & 0x0FU;
            minimum = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            codepoint = lead & 0x07U;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (length > text.size() - offset) {
            return false;
        }
        for (std::size_t index = 1; index < length; ++index) {
            const auto continuation = static_cast<unsigned char>(text[offset + index]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3FU);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFU
            || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
        offset += length;
    }
    return true;
}

[[nodiscard]] std::expected<void, Diagnostic>
select_size(detail::FontData& data, u32 pixel_size)
{
    if (pixel_size == 0 || pixel_size > maximum_pixel_size) {
        return std::unexpected(Diagnostic{
            ErrorCode::invalid_size, "text pixel size must be in [1, 4096]"});
    }
    if (data.current_size != pixel_size) {
        if (const auto error = FT_Set_Pixel_Sizes(data.face, 0, pixel_size)) {
            return std::unexpected(freetype_error(
                ErrorCode::invalid_size, "selecting the font pixel size", error));
        }
        hb_ft_font_changed(data.shaping_font);
        data.current_size = pixel_size;
    }
    return {};
}

[[nodiscard]] f32 pixels(FT_Pos value)
{
    return static_cast<f32>(static_cast<double>(value) / 64.0);
}

[[nodiscard]] bool representable(double value)
{
    return std::isfinite(value)
        && std::abs(value) <= static_cast<double>(std::numeric_limits<f32>::max());
}

using ShapingBuffer = std::unique_ptr<hb_buffer_t, decltype(&hb_buffer_destroy)>;

[[nodiscard]] std::expected<void, Diagnostic> append_run(
    detail::FontData& data,
    std::string_view source,
    std::size_t start,
    std::size_t length,
    double& cursor_x,
    double baseline,
    TextLayout& layout)
{
    if (length == 0) {
        return {};
    }
    ShapingBuffer buffer(hb_buffer_create(), &hb_buffer_destroy);
    if (!buffer || !hb_buffer_allocation_successful(buffer.get())) {
        return std::unexpected(Diagnostic{
            ErrorCode::shaping_failed, "could not allocate a text shaping buffer"});
    }
    hb_buffer_add_utf8(buffer.get(), source.data(), static_cast<int>(source.size()),
        static_cast<unsigned int>(start), static_cast<int>(length));
    hb_buffer_guess_segment_properties(buffer.get());
    hb_shape(data.shaping_font, buffer.get(), nullptr, 0);
    if (!hb_buffer_allocation_successful(buffer.get())) {
        return std::unexpected(Diagnostic{
            ErrorCode::shaping_failed, "text shaping ran out of memory"});
    }
    unsigned int count{};
    const auto* infos = hb_buffer_get_glyph_infos(buffer.get(), &count);
    const auto* positions = hb_buffer_get_glyph_positions(buffer.get(), nullptr);
    if (count != 0 && (!infos || !positions)) {
        return std::unexpected(Diagnostic{
            ErrorCode::shaping_failed, "text shaping produced an incomplete glyph run"});
    }
    if (count > maximum_text_bytes - layout.glyphs.size()) {
        return std::unexpected(Diagnostic{
            ErrorCode::text_too_large, "shaped text contains too many glyphs"});
    }
    double cursor_y = baseline;
    for (unsigned int index = 0; index < count; ++index) {
        const auto x = cursor_x + static_cast<double>(positions[index].x_offset) / 64.0;
        const auto y = cursor_y - static_cast<double>(positions[index].y_offset) / 64.0;
        if (!representable(x) || !representable(y)) {
            return std::unexpected(Diagnostic{
                ErrorCode::text_too_large, "text glyph position is not representable"});
        }
        layout.glyphs.push_back({
            .glyph_index = infos[index].codepoint,
            .position = {static_cast<f32>(x), static_cast<f32>(y)},
            .cluster = infos[index].cluster,
            .pen = {static_cast<f32>(cursor_x), static_cast<f32>(cursor_y)},
            .advance = {static_cast<f32>(positions[index].x_advance) / 64.0F,
                -static_cast<f32>(positions[index].y_advance) / 64.0F},
            .right_to_left = HB_DIRECTION_IS_BACKWARD(hb_buffer_get_direction(buffer.get())),
        });
        cursor_x += static_cast<double>(positions[index].x_advance) / 64.0;
        cursor_y -= static_cast<double>(positions[index].y_advance) / 64.0;
    }
    if (!representable(cursor_x)) {
        return std::unexpected(Diagnostic{
            ErrorCode::text_too_large, "text advance width is not representable"});
    }
    return {};
}

} // namespace

std::expected<Font, Diagnostic> Font::load(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return std::unexpected(Diagnostic{
            ErrorCode::file_read_failed, "could not open font: " + path.string()});
    }
    const auto end = stream.tellg();
    if (end <= 0 || end > static_cast<std::streamoff>(maximum_font_bytes)
        || end > static_cast<std::streamoff>(std::numeric_limits<FT_Long>::max())) {
        return std::unexpected(Diagnostic{
            ErrorCode::invalid_font, "font file must contain between 1 byte and 64 MiB"});
    }
    auto data = std::make_shared<detail::FontData>();
    data->bytes.resize(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(data->bytes.data()),
                     static_cast<std::streamsize>(data->bytes.size()))) {
        return std::unexpected(Diagnostic{
            ErrorCode::file_read_failed, "could not read font: " + path.string()});
    }
    if (const auto error = FT_Init_FreeType(&data->library)) {
        return std::unexpected(freetype_error(
            ErrorCode::invalid_font, "initializing FreeType", error));
    }
    if (const auto error = FT_New_Memory_Face(data->library, data->bytes.data(),
            static_cast<FT_Long>(data->bytes.size()), 0, &data->face)) {
        return std::unexpected(freetype_error(
            ErrorCode::invalid_font, "loading font", error));
    }
    if (!FT_IS_SCALABLE(data->face)) {
        return std::unexpected(Diagnostic{
            ErrorCode::invalid_font, "text rendering requires a scalable outline font"});
    }
    if (const auto error = FT_Select_Charmap(data->face, FT_ENCODING_UNICODE)) {
        return std::unexpected(freetype_error(
            ErrorCode::invalid_font, "selecting the font Unicode character map", error));
    }
    if (const auto error = FT_Set_Pixel_Sizes(data->face, 0, 16)) {
        return std::unexpected(freetype_error(
            ErrorCode::invalid_font, "initializing the font pixel size", error));
    }
    data->current_size = 16;
    data->shaping_font = hb_ft_font_create_referenced(data->face);
    if (!data->shaping_font || data->shaping_font == hb_font_get_empty()) {
        return std::unexpected(Diagnostic{
            ErrorCode::invalid_font, "could not create a shaping font"});
    }
    hb_ft_font_set_load_flags(data->shaping_font,
        FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL);
    data->identity = next_identity.fetch_add(1, std::memory_order_relaxed);
    return Font{std::move(data)};
}

bool Font::valid() const noexcept
{
    return static_cast<bool>(data_);
}

std::uint64_t Font::id() const noexcept
{
    return data_ ? data_->identity : 0;
}

std::expected<TextLayout, Diagnostic> Font::layout(std::string_view utf8, u32 pixel_size) const
{
    if (!data_) {
        return std::unexpected(Diagnostic{ErrorCode::invalid_font, "font is not loaded"});
    }
    if (utf8.size() > maximum_text_bytes) {
        return std::unexpected(Diagnostic{
            ErrorCode::text_too_large, "text must not exceed 16 MiB of UTF-8"});
    }
    if (!valid_utf8(utf8)) {
        return std::unexpected(Diagnostic{ErrorCode::invalid_utf8, "text is not valid UTF-8"});
    }
    std::scoped_lock lock(data_->mutex);
    if (auto selected = select_size(*data_, pixel_size); !selected) {
        return std::unexpected(selected.error());
    }
    TextLayout result{};
    const auto& font_metrics = data_->face->size->metrics;
    result.metrics.baseline = std::max(0.0F, pixels(font_metrics.ascender));
    result.metrics.line_height = std::max({
        pixels(font_metrics.height),
        pixels(font_metrics.ascender) - pixels(font_metrics.descender),
        1.0F,
    });
    if (!std::isfinite(result.metrics.baseline) || !std::isfinite(result.metrics.line_height)) {
        return std::unexpected(Diagnostic{
            ErrorCode::invalid_font, "font line metrics are not representable"});
    }
    if (utf8.empty()) {
        return result;
    }

    // Derive tab width through the same shaping path as visible text.
    TextLayout space{};
    double space_advance{};
    if (auto shaped = append_run(*data_, " ", 0, 1, space_advance, 0.0, space); !shaped) {
        return std::unexpected(shaped.error());
    }
    const auto tab_width = std::max(space_advance, 1.0) * 4.0;

    result.metrics.line_count = 1;
    double cursor_x{};
    double baseline = result.metrics.baseline;
    std::size_t start{};
    while (start < utf8.size()) {
        auto end = utf8.find_first_of("\t\r\n", start);
        if (end == std::string_view::npos) {
            end = utf8.size();
        }
        if (auto shaped = append_run(*data_, utf8, start, end - start,
                cursor_x, baseline, result); !shaped) {
            return std::unexpected(shaped.error());
        }
        result.metrics.width = std::max(result.metrics.width, static_cast<f32>(cursor_x));
        if (end == utf8.size()) {
            break;
        }
        if (utf8[end] == '\t') {
            cursor_x = (std::floor(cursor_x / tab_width) + 1.0) * tab_width;
            result.metrics.width = std::max(result.metrics.width, static_cast<f32>(cursor_x));
        } else {
            ++result.metrics.line_count;
            cursor_x = 0.0;
            baseline += result.metrics.line_height;
            if (utf8[end] == '\r' && end + 1 < utf8.size() && utf8[end + 1] == '\n') {
                ++end;
            }
        }
        start = end + 1;
    }
    const auto height = static_cast<double>(result.metrics.line_count) * result.metrics.line_height;
    if (!representable(height)) {
        return std::unexpected(Diagnostic{
            ErrorCode::text_too_large, "text line-box height is not representable"});
    }
    result.metrics.height = static_cast<f32>(height);
    return result;
}

std::expected<TextMetrics, Diagnostic> Font::measure(std::string_view utf8, u32 pixel_size) const
{
    auto shaped = layout(utf8, pixel_size);
    if (!shaped) {
        return std::unexpected(shaped.error());
    }
    return shaped->metrics;
}

std::expected<GlyphBitmap, Diagnostic> Font::rasterize(u32 glyph_index, u32 pixel_size) const
{
    if (!data_) {
        return std::unexpected(Diagnostic{ErrorCode::invalid_font, "font is not loaded"});
    }
    std::scoped_lock lock(data_->mutex);
    if (auto selected = select_size(*data_, pixel_size); !selected) {
        return std::unexpected(selected.error());
    }
    if (static_cast<std::uint64_t>(glyph_index)
        >= static_cast<std::uint64_t>(data_->face->num_glyphs)) {
        return std::unexpected(Diagnostic{
            ErrorCode::rasterization_failed, "glyph index is outside this font"});
    }
    if (const auto error = FT_Load_Glyph(data_->face, glyph_index,
            FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL)) {
        return std::unexpected(freetype_error(
            ErrorCode::rasterization_failed, "loading glyph outline", error));
    }
    if (const auto error = FT_Render_Glyph(data_->face->glyph, FT_RENDER_MODE_NORMAL)) {
        return std::unexpected(freetype_error(
            ErrorCode::rasterization_failed, "rasterizing glyph", error));
    }
    const auto& slot = *data_->face->glyph;
    const auto& bitmap = slot.bitmap;
    GlyphBitmap result{
        .width = bitmap.width,
        .height = bitmap.rows,
        .left = slot.bitmap_left,
        .top = slot.bitmap_top,
    };
    if (result.width == 0 || result.height == 0) {
        return result;
    }
    const auto byte_count = static_cast<std::uint64_t>(result.width) * result.height;
    if (byte_count > maximum_bitmap_bytes || !bitmap.buffer
        || bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.num_grays < 2) {
        return std::unexpected(Diagnostic{
            ErrorCode::rasterization_failed, "glyph did not produce supported 8-bit grayscale coverage"});
    }
    const auto pitch = static_cast<std::int64_t>(bitmap.pitch);
    if (std::abs(pitch) < result.width) {
        return std::unexpected(Diagnostic{
            ErrorCode::rasterization_failed, "glyph bitmap row pitch is invalid"});
    }
    result.pixels.resize(static_cast<std::size_t>(byte_count));
    for (u32 row = 0; row < result.height; ++row) {
        const auto source_row = pitch >= 0 ? row : result.height - row - 1;
        const auto* source = bitmap.buffer + static_cast<std::size_t>(source_row * std::abs(pitch));
        auto* destination = result.pixels.data() + static_cast<std::size_t>(row) * result.width;
        if (bitmap.num_grays == 256) {
            std::copy_n(source, result.width, destination);
        } else {
            for (u32 column = 0; column < result.width; ++column) {
                destination[column] = static_cast<std::uint8_t>(
                    static_cast<u32>(source[column]) * 255U / (bitmap.num_grays - 1U));
            }
        }
    }
    return result;
}

} // namespace vng::text
