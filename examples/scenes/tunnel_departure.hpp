#pragma once
#include "tunnel_scene.hpp"
#include "departure_voyage.hpp"
#include "../support/earth_express_route.hpp"

// The skyway act: the courier's run down the express tunnel and out of the
// dispersal terminal. The voyage into space continues from the handoff.
namespace example::tunnel::departure {
inline constexpr vng::f32 duration=voyage::duration;
inline constexpr vng::u32 hero=2, terminal=3, earth=4;
// The authored Earth route is above the 1.024-radius endpoint shell.
// All scene dimensions remain kilometres.
inline constexpr vng::Vec3 earth_center=example::earth::express_route::center;
inline constexpr vng::f32 earth_radius=6371;
inline constexpr vng::u32 camera_id=1001;
inline constexpr vng::f32 initial_speed=2.2F*1.3F*1.2F,start_distance=141.408F;
[[nodiscard]] constexpr vng::f32 smooth(vng::f32 u){return u*u*(3-2*u);}
[[nodiscard]] constexpr vng::f32 smooth_integral(vng::f32 u){return u*u*u*(1-u*.5F);}
// Shift the acceleration envelope two seconds earlier and multiply it by 1.4.
// Subtract its initial velocity contribution to retain the authored start speed.
inline constexpr vng::f32 acceleration_gain=12.F*1.4F,acceleration_lead=2.F,ramp_duration=40.F;
[[nodiscard]] constexpr vng::f32 ramp(vng::f32 t){return t+acceleration_lead<ramp_duration?(t+acceleration_lead)/ramp_duration:1.F;}
[[nodiscard]] constexpr vng::f32 speed(vng::f32 t){return initial_speed+acceleration_gain*(smooth(ramp(t))-smooth(ramp(0)));}
[[nodiscard]] constexpr vng::f32 travel(vng::f32 t){
    return initial_speed*t+acceleration_gain*(ramp_duration*(smooth_integral(ramp(t))-smooth_integral(ramp(0)))
        -smooth(ramp(0))*t+(t>ramp_duration-acceleration_lead?t-(ramp_duration-acceleration_lead):0.F));
}
[[nodiscard]] constexpr vng::f32 time_at_distance(vng::f32 distance) {
    vng::f32 low=0,high=duration;
    for(unsigned i=0;i<24;++i){const auto mid=(low+high)*.5F;if(travel(mid)<distance)low=mid;else high=mid;}
    return (low+high)*.5F;
}
inline constexpr vng::f32 throat_time=time_at_distance(start_distance),exit_time=time_at_distance(start_distance+12);
inline constexpr vng::f32 camera_distance_scale=.7F;
// The opening: a camera waits in the empty bore. Traffic comes in past it,
// then the courier, far faster, passes the camera's station at
// courier_arrival; a moment later the camera races after it, settling into
// the chase at the end of the catch-up.
inline constexpr vng::f32 courier_arrival=5.6F,catch_up_begin=6.1F,catch_up_end=6.6F;
// The one move inside the tube: an eased swing from the chase to the close
// side-offset exit approach, which carries straight on into the voyage.
inline constexpr vng::f32 exit_approach=throat_time-1.4F,exit_approach_duration=.8F;
// The voyage takes the courier and camera just after the mouth.
inline constexpr vng::f32 handoff_time=exit_time+.8F;
// The skyway route: the courier's flight until the handoff.
[[nodiscard]] vng::Vec3 flight(vng::f32 time);
[[nodiscard]] vng::Vec3 velocity(vng::f32 time);
[[nodiscard]] vng::content::Result<editor_example::State> author_scene(const std::filesystem::path& assets);
}
