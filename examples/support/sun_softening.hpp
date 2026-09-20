#pragma once

#include <memory>
#include <vng/resources/diagnostic.hpp>

namespace vng::opengl { class Device; class Frame; class Image2D; }

namespace example::sun {

// A gentle, fixed screen-space Gaussian for the composed sun. The caller
// supplies a separate, same-sized linear HDR destination before bloom.
class SunSoftening final {
public:
    [[nodiscard]] static vng::resources::Result<SunSoftening> create(vng::opengl::Device&);
    SunSoftening(SunSoftening&&) noexcept;
    SunSoftening& operator=(SunSoftening&&) noexcept;
    SunSoftening(const SunSoftening&) = delete;
    SunSoftening& operator=(const SunSoftening&) = delete;
    ~SunSoftening();

    // Preserves native render and sampling state; frame handles remain valid.
    // Reselect the next program/view and graphics settings after this pass.
    // No shader or geometry is rebuilt when dimensions change.
    [[nodiscard]] vng::resources::Result<void> apply(
        vng::opengl::Frame& output, const vng::opengl::Image2D& linear_hdr);

private:
    struct Impl;
    explicit SunSoftening(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace example::sun
