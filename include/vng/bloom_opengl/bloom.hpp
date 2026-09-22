#pragma once

#include <memory>

#include <vng/render/bloom.hpp>
#include <vng/resources/resources.hpp>

namespace vng::opengl {
class Device;
class Frame;
class Image2D;

// Owns its reconstruction recipes, fixed DSL programs, fullscreen triangle,
// and HDR target pyramid. resize() replaces targets only; reload() prepares all
// resources before committing. Both require a safe point outside any Frame.
class Bloom final {
public:
    [[nodiscard]] static resources::Result<Bloom> create(
        const Device& device, Extent2D extent, render::BloomOptions options = {});

    Bloom(Bloom&&) noexcept;
    Bloom& operator=(Bloom&&) noexcept;
    ~Bloom();
    Bloom(const Bloom&) = delete;
    Bloom& operator=(const Bloom&) = delete;

    [[nodiscard]] Extent2D extent() const noexcept;
    [[nodiscard]] u32 levels() const noexcept;
    [[nodiscard]] resources::Result<void> resize(const Device& device, Extent2D extent);
    [[nodiscard]] resources::Result<void> reload(const Device& device);

    // Composite an RGBA16F/RGBA32F linear HDR image into color output 0 of an
    // active frame of matching extent, preserving source alpha and additional
    // attachments. Output 0 must be normalized or floating point. Input must
    // not alias an image attached to that frame. Restores GL state and units;
    // frame command handles stay valid. Reselect the next program/view and
    // required graphics settings after the explicit native-state scope.
    // Tone mapping is Reinhard; the frame controls the final color encoding.
    [[nodiscard]] resources::Result<void> apply(
        Frame& output, const Image2D& linear_hdr,
        render::BloomSettings settings = {});

private:
    struct Impl;
    explicit Bloom(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

class BloomBuilder final {
public:
    explicit BloomBuilder(const Device& device) noexcept : device_(&device) {}
    BloomBuilder& levels(u32 count) noexcept { options_.levels = count; return *this; }
    [[nodiscard]] resources::Result<Bloom> build(Extent2D extent) const;

private:
    const Device* device_;
    render::BloomOptions options_{};
};

[[nodiscard]] inline BloomBuilder make_backend_bloom_builder(const Device& device) noexcept
{
    return BloomBuilder{device};
}

} // namespace vng::opengl
