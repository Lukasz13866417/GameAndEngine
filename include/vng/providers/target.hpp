#pragma once

#include <vng/render/target.hpp>
#include <vng/resources/diagnostic.hpp>

namespace vng::providers {

class RenderTargetProvider final {
public:
    explicit RenderTargetProvider(render::TargetDesc description = {}) noexcept
        : description_(description) {}

    template<class Device>
    [[nodiscard]] auto provide(Device& device, Extent2D extent) const
        -> decltype(resources::into_result(render::make_target(device, render::TargetDesc{}, extent)))
    {
        return resources::into_result(render::make_target(device, description_, extent));
    }

private:
    render::TargetDesc description_;
};

[[nodiscard]] inline RenderTargetProvider render_target(render::TargetDesc description = {})
{
    return RenderTargetProvider{description};
}

} // namespace vng::providers
