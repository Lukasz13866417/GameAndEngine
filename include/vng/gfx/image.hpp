#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <utility>
#include <vector>

#include <vng/core/types.hpp>

namespace vng::gfx {

enum class ImageFormat {
    r8, rgba8, srgb8_alpha8, rgba16f, rg32ui, rgba32f, rgba32i, rgba32ui, depth32f,
};

[[nodiscard]] constexpr bool is_color_format(ImageFormat format) noexcept
{
    switch (format) {
    case ImageFormat::r8: case ImageFormat::rgba8: case ImageFormat::srgb8_alpha8:
    case ImageFormat::rgba16f: case ImageFormat::rg32ui: case ImageFormat::rgba32f:
    case ImageFormat::rgba32i: case ImageFormat::rgba32ui: return true;
    default: return false;
    }
}

[[nodiscard]] constexpr bool is_depth_format(ImageFormat format) noexcept
{
    return format == ImageFormat::depth32f;
}

[[nodiscard]] constexpr bool is_integer_format(ImageFormat format) noexcept
{
    return format == ImageFormat::rg32ui || format == ImageFormat::rgba32i
        || format == ImageFormat::rgba32ui;
}

enum class ImageFilter { nearest, linear };
enum class MipmapFilter { none, nearest, linear };
enum class ImageWrap { clamp_to_edge, repeat, mirrored_repeat };

struct SamplerDesc final {
    ImageFilter min_filter{ImageFilter::nearest};
    ImageFilter mag_filter{ImageFilter::nearest};
    MipmapFilter mip_filter{MipmapFilter::none};
    ImageWrap wrap_u{ImageWrap::clamp_to_edge};
    ImageWrap wrap_v{ImageWrap::clamp_to_edge};
    friend constexpr bool operator==(const SamplerDesc&, const SamplerDesc&) = default;
};

[[nodiscard]] constexpr u32 full_mip_count(Extent2D extent) noexcept
{
    return extent.empty() ? 0U : std::bit_width(std::max(extent.width, extent.height));
}

struct ImageDesc final {
    Extent2D extent{};
    ImageFormat format{ImageFormat::rgba8};
    u32 mip_levels{1};
    SamplerDesc sampler{};
};

// Owned, tightly packed RGBA8 pixels. Row zero is the top image row, as in
// PNG/PPM files. Texture coordinates choose whether a consumer flips Y.
struct ImageData final {
    Extent2D extent{};
    std::vector<std::byte> pixels;
};

struct ImageUploadOptions final {
    ImageFormat format{ImageFormat::srgb8_alpha8};
    bool generate_mipmaps{true};
    SamplerDesc sampler{
        .min_filter = ImageFilter::linear,
        .mag_filter = ImageFilter::linear,
        .mip_filter = MipmapFilter::linear,
    };
};

// Backend selection follows the device through ADL. Providers can retain
// these neutral descriptions without depending on a concrete graphics API.
struct MakeImage final {
    template<class Device>
    [[nodiscard]] auto operator()(Device& device, const ImageDesc& description) const
        -> decltype(make_backend_image(device, description))
    {
        return make_backend_image(device, description);
    }
};
inline constexpr MakeImage make_image{};

struct UploadImage final {
    template<class Device>
    [[nodiscard]] auto operator()(Device& device, const ImageData& data,
        const ImageUploadOptions& options = {}) const
        -> decltype(upload_backend_image(device, data, options))
    {
        return upload_backend_image(device, data, options);
    }
};
inline constexpr UploadImage upload_image{};

} // namespace vng::gfx
