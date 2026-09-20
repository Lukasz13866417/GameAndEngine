#pragma once

#include <vng/gfx/image.hpp>

namespace vng::render {

struct TargetDesc final {
    gfx::ImageFormat color{gfx::ImageFormat::rgba16f};
    bool depth{true};
    friend constexpr bool operator==(const TargetDesc&, const TargetDesc&) = default;
};

struct MakeTarget final {
    template<class Device>
    [[nodiscard]] auto operator()(Device& device, const TargetDesc& description,
        Extent2D extent) const -> decltype(make_backend_target(device, description, extent))
    {
        return make_backend_target(device, description, extent);
    }
};
inline constexpr MakeTarget make_target{};

} // namespace vng::render
