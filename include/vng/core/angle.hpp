#pragma once

#include <numbers>

#include <vng/core/types.hpp>

namespace vng {

class Angle final {
public:
    constexpr Angle() noexcept = default;

    [[nodiscard]] constexpr f32 radians() const noexcept
    {
        return radians_;
    }

    [[nodiscard]] constexpr f32 degrees() const noexcept
    {
        return radians_ * (180.0F / std::numbers::pi_v<f32>);
    }

    friend constexpr bool operator==(Angle, Angle) = default;

private:
    explicit constexpr Angle(f32 radians) noexcept : radians_(radians) {}

    f32 radians_{};

    friend constexpr Angle radians(f32 value) noexcept;
};

[[nodiscard]] constexpr Angle radians(f32 value) noexcept
{
    return Angle{value};
}

[[nodiscard]] constexpr Angle degrees(f32 value) noexcept
{
    return radians(value * (std::numbers::pi_v<f32> / 180.0F));
}

} // namespace vng

namespace vng::core {

using ::vng::Angle;
using ::vng::degrees;
using ::vng::radians;

} // namespace vng::core
