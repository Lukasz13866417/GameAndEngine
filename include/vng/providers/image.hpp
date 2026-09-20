#pragma once

#include <vng/content/image.hpp>
#include <vng/gfx/image.hpp>
#include <vng/resources/resources.hpp>

namespace vng::providers {

struct TextureFileProvider {
    std::filesystem::path path;
    gfx::ImageUploadOptions options;
    template<class Device>
    auto provide(Device& device) const
    {
        using Image = typename decltype(gfx::upload_image(device, std::declval<const gfx::ImageData&>(), options))::value_type;
        auto pixels = content::load_image(path);
        if (!pixels) return resources::Result<Image>{std::unexpected(resources::to_diagnostic(pixels.error()))};
        return resources::into_result(gfx::upload_image(device, *pixels, options));
    }
};
inline TextureFileProvider texture_file(std::filesystem::path path, gfx::ImageUploadOptions options = {})
{ return {std::move(path), options}; }

struct TextureProvider {
    std::shared_ptr<const gfx::ImageData> pixels;
    gfx::ImageUploadOptions options;
    template<class Device> auto provide(Device& device) const
    { return gfx::upload_image(device, *pixels, options); }
};
inline TextureProvider texture(gfx::ImageData pixels, gfx::ImageUploadOptions options = {})
{ return {std::make_shared<const gfx::ImageData>(std::move(pixels)), options}; }

} // namespace vng::providers
