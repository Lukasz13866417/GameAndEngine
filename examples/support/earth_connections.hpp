#pragma once
#include "earth_infrastructure.hpp"
#include "earth_skyway.hpp"
#include <span>
#include <array>

namespace example::earth {
// Unbaked Earth-space socket frames. The same recipe supplies geometry,
// attachment positions, and editor choices; no duplicated magic positions.
struct TunnelSocket {
    TunnelSocketRef id;
    std::string label;
    vng::Vec3 position, outward, up;
    vng::f32 size{1};
};
[[nodiscard]] std::vector<TunnelSocket> infrastructure_sockets(
    std::span<const InfrastructurePart>,InfrastructureSettings);
[[nodiscard]] vng::content::Result<void> validate_infrastructure_connections(
    std::span<const InfrastructurePart>,InfrastructureSettings);

// Automatic routes use a great-circle arch or socket-matching Hermite path.
// Explicit Bezier routes use the same endpoints but wholly authored controls.
class TunnelCurve {
public:
    [[nodiscard]] static vng::content::Result<TunnelCurve> create(
        const InfrastructurePart&,std::span<const InfrastructurePart>,InfrastructureSettings);
    [[nodiscard]] SkywaySample sample(vng::f32) const;
    [[nodiscard]] vng::f32 size(vng::f32) const;
    [[nodiscard]] std::vector<vng::Vec3> initial_bezier_controls() const;
    // Nearly uniform arc-length spacing, with half an interval clear at each
    // end. Invalid/excessive densities fail before allocating any geometry.
    [[nodiscard]] vng::content::Result<std::vector<vng::f32>> scaffold_parameters(
        vng::f32 spacing,vng::f32 begin=0,vng::f32 end=1) const;
private:
    explicit TunnelCurve(SkywayCurve curve):ground_(std::move(curve)){}
    [[nodiscard]] vng::Vec3 position(vng::f32) const;
    SkywayCurve ground_;
    bool attached_{};
    std::optional<std::vector<vng::Vec3>> bezier_;
    vng::Vec3 a_{},b_{},da_{},db_{},up_a_{},up_b_{};
    vng::f32 size_a_{1},size_b_{1},size_{1},rise_{};
};
// Arc-length mapping shared by spacing controls and uniform placement.
class TunnelArc {
public:
    explicit TunnelArc(const TunnelCurve&);
    vng::f32 distance(vng::f32 parameter) const;
    vng::f32 parameter(vng::f32 distance) const;
    vng::f32 length() const {return distances_.back();}
private:
    std::array<vng::f32,129> distances_{};
};
// Keep existing endpoint supports; include first/last exactly and distribute
// interior supports evenly by arc length, with gaps no larger than spacing.
[[nodiscard]] vng::content::Result<std::vector<vng::f32>> uniform_tunnel_supports(
    const TunnelArc&,vng::f32 first,vng::f32 last,vng::f32 spacing,std::span<const vng::f32> endpoints);
// Shared by geometry and editor handles, including the legacy end supports.
// Does not hide positions when Scaffolding is off: toggling it retains edits.
[[nodiscard]] vng::content::Result<std::vector<vng::f32>> tunnel_support_positions(
    const InfrastructurePart&, const TunnelCurve&);
// Short flared fittings measured in bore diameters, not a fraction of a long
// intercontinental route. Shared by mesh generation and camera/flight sampling.
[[nodiscard]] vng::Vec2 tunnel_body_range(const InfrastructurePart&,const TunnelCurve&);
[[nodiscard]] inline vng::f32 tunnel_terminal_rise(const InfrastructurePart& part) {
    return .5F*std::tan(part.terminal_incline*placement::radians);
}
[[nodiscard]] SkywaySample tunnel_terminal_sample(const InfrastructurePart&,const TunnelCurve&,bool end,vng::f32);
}
