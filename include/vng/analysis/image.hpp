#pragma once

#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <vng/analysis/types.hpp>

namespace vng::analysis {

// Images exposed by analysis use top-left coordinates regardless of the
// originating graphics API. A backend performs any required row flip while
// materializing an Image.
template<class T>
class Image final {
public:
    using pixel_type = T;
    using container_type = std::vector<T>;
    using size_type = typename container_type::size_type;

    Image() = default;

    explicit Image(Extent2D extent)
        : extent_(extent), pixels_(pixel_count(extent))
    {}

    Image(Extent2D extent, const T& value)
        : extent_(extent), pixels_(pixel_count(extent), value)
    {}

    Image(Extent2D extent, container_type pixels)
        : extent_(extent), pixels_(std::move(pixels))
    {
        if (pixels_.size() != pixel_count(extent_)) {
            throw std::invalid_argument(
                "analysis image pixel count does not match its extent");
        }
    }

    [[nodiscard]] Extent2D extent() const noexcept { return extent_; }
    [[nodiscard]] u32 width() const noexcept { return extent_.width; }
    [[nodiscard]] u32 height() const noexcept { return extent_.height; }
    [[nodiscard]] bool empty() const noexcept { return pixels_.empty(); }
    [[nodiscard]] size_type size() const noexcept { return pixels_.size(); }

    [[nodiscard]] bool contains(Pixel pixel) const noexcept
    {
        return pixel.x < extent_.width && pixel.y < extent_.height;
    }

    [[nodiscard]] T& at(Pixel pixel)
    {
        return pixels_.at(linear_index(pixel));
    }

    [[nodiscard]] const T& at(Pixel pixel) const
    {
        return pixels_.at(linear_index(pixel));
    }

    [[nodiscard]] std::span<T> row(u32 y)
    {
        if (y >= extent_.height) {
            throw std::out_of_range("analysis image row is out of bounds");
        }
        const auto begin = static_cast<size_type>(y)
            * static_cast<size_type>(extent_.width);
        return std::span<T>{pixels_}.subspan(begin, extent_.width);
    }

    [[nodiscard]] std::span<const T> row(u32 y) const
    {
        if (y >= extent_.height) {
            throw std::out_of_range("analysis image row is out of bounds");
        }
        const auto begin = static_cast<size_type>(y)
            * static_cast<size_type>(extent_.width);
        return std::span<const T>{pixels_}.subspan(begin, extent_.width);
    }

    [[nodiscard]] std::span<T> pixels() noexcept { return pixels_; }
    [[nodiscard]] std::span<const T> pixels() const noexcept { return pixels_; }

private:
    [[nodiscard]] static size_type pixel_count(Extent2D extent)
    {
        const auto width = static_cast<size_type>(extent.width);
        const auto height = static_cast<size_type>(extent.height);
        if (width != 0
            && height > std::numeric_limits<size_type>::max() / width) {
            throw std::length_error("analysis image extent is too large");
        }
        return width * height;
    }

    [[nodiscard]] size_type linear_index(Pixel pixel) const
    {
        if (!contains(pixel)) {
            throw std::out_of_range("analysis image pixel is out of bounds");
        }
        return static_cast<size_type>(pixel.y)
                * static_cast<size_type>(extent_.width)
            + static_cast<size_type>(pixel.x);
    }

    Extent2D extent_{};
    container_type pixels_;
};

} // namespace vng::analysis
