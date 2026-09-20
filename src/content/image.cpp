#include <vng/content/image.hpp>

#include <png.h>

#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>

namespace vng::content {
namespace {

Diagnostic error(ErrorCode code, std::string message)
{
    return Diagnostic{.code = code, .message = std::move(message),
        .location = {}, .path = {}, .notes = {}};
}

Result<std::size_t> decoded_size(Extent2D extent, const ImageLoadLimits& limits)
{
    const u64 pixels = static_cast<u64>(extent.width) * extent.height;
    if (extent.empty() || extent.width > limits.max_dimension || extent.height > limits.max_dimension
        || pixels > limits.max_decoded_bytes / 4
        || pixels > std::numeric_limits<std::size_t>::max() / 4)
        return std::unexpected(error(ErrorCode::limit_exceeded,
            "Image dimensions exceed the configured decoded-image limits"));
    return static_cast<std::size_t>(pixels * 4);
}

Result<gfx::ImageData> decode_png(std::span<const std::byte> bytes, const ImageLoadLimits& limits)
{
    struct Png final {
        png_image value{};
        Png() { value.version = PNG_IMAGE_VERSION; }
        ~Png() { png_image_free(&value); }
    } png;
    if (!png_image_begin_read_from_memory(&png.value, bytes.data(), bytes.size()))
        return std::unexpected(error(ErrorCode::invalid_document,
            std::string("PNG header: ") + png.value.message));
    const Extent2D extent{png.value.width, png.value.height};
    auto size = decoded_size(extent, limits);
    if (!size) return std::unexpected(std::move(size.error()));
    png.value.format = PNG_FORMAT_RGBA;
    gfx::ImageData data{.extent = extent, .pixels = std::vector<std::byte>(*size)};
    if (!png_image_finish_read(&png.value, nullptr, data.pixels.data(), 0, nullptr))
        return std::unexpected(error(ErrorCode::invalid_document,
            std::string("PNG pixels: ") + png.value.message));
    return data;
}

bool whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

Result<gfx::ImageData> decode_ppm(std::span<const std::byte> bytes, const ImageLoadLimits& limits)
{
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t cursor = 2;
    const auto number = [&]() -> Result<u32> {
        while (cursor < text.size()) {
            if (whitespace(text[cursor])) { ++cursor; continue; }
            if (text[cursor] != '#') break;
            while (cursor < text.size() && text[cursor] != '\n') ++cursor;
        }
        const auto begin = cursor;
        while (cursor < text.size() && !whitespace(text[cursor]) && text[cursor] != '#') ++cursor;
        u32 value{};
        const auto parsed = std::from_chars(text.data() + begin, text.data() + cursor, value);
        if (begin == cursor || parsed.ec != std::errc{} || parsed.ptr != text.data() + cursor)
            return std::unexpected(error(ErrorCode::invalid_number, "PPM header contains an invalid unsigned integer"));
        return value;
    };
    auto width = number();
    if (!width) return std::unexpected(std::move(width.error()));
    auto height = number();
    if (!height) return std::unexpected(std::move(height.error()));
    auto maximum = number();
    if (!maximum) return std::unexpected(std::move(maximum.error()));
    if (*maximum == 0 || *maximum > 255)
        return std::unexpected(error(ErrorCode::unsupported_version,
            "P6 PPM supports 8-bit samples with maximum values in [1, 255]"));
    if (cursor == text.size() || !whitespace(text[cursor]))
        return std::unexpected(error(ErrorCode::invalid_document, "PPM header is missing its pixel-data separator"));
    const char separator = text[cursor++];
    if (separator == '\r' && cursor < text.size() && text[cursor] == '\n') ++cursor;
    const Extent2D extent{*width, *height};
    auto size = decoded_size(extent, limits);
    if (!size) return std::unexpected(std::move(size.error()));
    const auto source_size = *size / 4 * 3;
    if (text.size() - cursor != source_size)
        return std::unexpected(error(ErrorCode::count_mismatch,
            "PPM pixel data does not match its dimensions (one image is required)"));
    gfx::ImageData data{.extent = extent, .pixels = std::vector<std::byte>(*size)};
    for (std::size_t pixel = 0; pixel < *size / 4; ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto sample = std::to_integer<u32>(bytes[cursor + pixel * 3 + channel]);
            if (sample > *maximum)
                return std::unexpected(error(ErrorCode::number_out_of_range,
                    "PPM pixel sample exceeds its declared maximum"));
            data.pixels[pixel * 4 + channel] = static_cast<std::byte>((sample * 255 + *maximum / 2) / *maximum);
        }
        data.pixels[pixel * 4 + 3] = std::byte{255};
    }
    return data;
}

} // namespace

Result<gfx::ImageData> decode_image(std::span<const std::byte> bytes, const ImageLoadLimits& limits)
{
    if (bytes.size() > limits.max_file_bytes)
        return std::unexpected(error(ErrorCode::input_too_large, "Encoded image exceeds the configured file-size limit"));
    if (bytes.size() >= 8 && png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes.data()), 0, 8) == 0)
        return decode_png(bytes, limits);
    if (bytes.size() >= 3 && bytes[0] == std::byte{'P'} && bytes[1] == std::byte{'6'}
        && whitespace(static_cast<char>(bytes[2])))
        return decode_ppm(bytes, limits);
    return std::unexpected(error(ErrorCode::invalid_document,
        "Unrecognized image encoding; expected PNG or binary P6 PPM"));
}

Result<gfx::ImageData> load_image(const std::filesystem::path& path, const ImageLoadLimits& limits)
{
    const auto with_path = [&](Diagnostic diagnostic) -> Result<gfx::ImageData> {
        diagnostic.path = path;
        return std::unexpected(std::move(diagnostic));
    };
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return with_path(error(ErrorCode::io_error, "Unable to open image file"));
    const auto length = stream.tellg();
    if (length < 0) return with_path(error(ErrorCode::io_error, "Unable to determine image file size"));
    if (static_cast<std::uintmax_t>(length) > limits.max_file_bytes
        || static_cast<std::uintmax_t>(length) > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
        return with_path(error(ErrorCode::input_too_large, "Image file exceeds the configured file-size limit"));
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return with_path(error(ErrorCode::io_error, "Unable to read complete image file"));
    auto decoded = decode_image(bytes, limits);
    if (!decoded) return with_path(std::move(decoded.error()));
    return decoded;
}

} // namespace vng::content
