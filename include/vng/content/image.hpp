#pragma once

#include <cstddef>
#include <filesystem>
#include <span>

#include <vng/content/diagnostic.hpp>
#include <vng/gfx/image.hpp>

namespace vng::content {

struct ImageLoadLimits final {
    std::size_t max_file_bytes{256U * 1024U * 1024U};
    std::size_t max_decoded_bytes{256U * 1024U * 1024U};
    u32 max_dimension{16384};
};

// PNG and binary P6 PPM are decoded to tightly packed RGBA8, with top row
// first and straight (unpremultiplied) alpha. PNG alpha is preserved.
[[nodiscard]] Result<gfx::ImageData> decode_image(
    std::span<const std::byte> bytes, const ImageLoadLimits& limits = {});
[[nodiscard]] Result<gfx::ImageData> load_image(
    const std::filesystem::path& path, const ImageLoadLimits& limits = {});

} // namespace vng::content
