#pragma once

#include "earth_placement.hpp"

namespace example::earth {

inline constexpr vng::f32 skyway_endpoint_radius = 1.024F;
inline constexpr vng::f32 skyway_default_rise = .060F;

struct SkywaySample {
    vng::Vec3 position, tangent, side, up;
};

// CPU-only Earth-space recipe. Both endpoints lie on the same surface shell.
// Angular progress is uniform along the shorter great-circle route. A sine
// arch adds radial clearance, with an analytic tangent for the tube/rib frame.
class SkywayCurve {
public:
    [[nodiscard]] static vng::content::Result<SkywayCurve> create(
        vng::Vec2 start, vng::Vec2 end, vng::f32 rise = skyway_default_rise,
        vng::f32 start_radius=skyway_endpoint_radius,vng::f32 end_radius=skyway_endpoint_radius) {
        if (!placement::valid(start) || !placement::valid(end) || !std::isfinite(rise) || rise < 0 ||
            !std::isfinite(start_radius)||!std::isfinite(end_radius)||start_radius<skyway_endpoint_radius||end_radius<skyway_endpoint_radius)
            return std::unexpected(mesh_frame::error("Invalid tunnel endpoints or arch height"));
        SkywayCurve curve;
        curve.a_ = placement::unit(placement::direction(start));
        curve.b_ = placement::unit(placement::direction(end));
        const auto cross = placement::cross(curve.a_, curve.b_);
        const auto sine = std::hypot(cross.x, cross.y, cross.z);
        curve.angle_ = std::atan2(sine, placement::dot(curve.a_, curve.b_));
        // Coincident/antipodal anchors do not define a unique stable route.
        if (curve.angle_ < .1F * placement::radians || curve.angle_ > 150.F * placement::radians)
            return std::unexpected(mesh_frame::error("Tunnel endpoints must be 0.1–150 degrees apart"));
        curve.side_ = placement::mul(cross, 1.F / sine);
        curve.along_ = placement::cross(curve.side_, curve.a_);
        curve.rise_ = rise;
        curve.start_radius_=start_radius;curve.end_radius_=end_radius;
        return curve;
    }

    [[nodiscard]] SkywaySample sample(vng::f32 progress) const {
        using namespace placement;
        const auto t = std::clamp(progress, 0.F, 1.F);
        const auto theta = angle_ * t;
        const auto direction = t == 0 ? a_ : t == 1 ? b_
            : add(mul(a_, std::cos(theta)), mul(along_, std::sin(theta)));
        const auto derivative = mul(add(mul(a_, -std::sin(theta)), mul(along_, std::cos(theta))), angle_);
        constexpr auto pi = std::numbers::pi_v<vng::f32>;
        const auto radius = start_radius_+(end_radius_-start_radius_)*t + (t == 0 || t == 1 ? 0.F : rise_ * std::sin(pi * t));
        const auto radial_speed = end_radius_-start_radius_+rise_ * pi * std::cos(pi * t);
        const auto tangent = unit(add(mul(direction, radial_speed), mul(derivative, radius)));
        return {mul(direction, radius), tangent, side_, unit(cross(tangent, side_))};
    }

private:
    SkywayCurve() = default;
    vng::Vec3 a_{}, b_{}, side_{}, along_{};
    vng::f32 angle_{}, rise_{};
    vng::f32 start_radius_{skyway_endpoint_radius},end_radius_{skyway_endpoint_radius};
};

} // namespace example::earth
