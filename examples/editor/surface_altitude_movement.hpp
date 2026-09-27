#pragma once
#include "surface_move.hpp"
#include "../support/mesh_frame.hpp"
#include <algorithm>

namespace editor_example {
// SurfaceMove remains blueprint-local. Only this bridge interprets a world
// axis drag as a radius change, including rotated/non-uniform blueprint frames.
struct SurfaceAltitudeMovement {
    struct Target {
        const SurfaceMove& surface;
        vng::Mat4 inverse;
        vng::Vec3 radial; // Validated local unit direction; radial_range exists.
    };
    struct Edit { vng::Vec3 local_position; };
    static vng::Vec3 world_position(const Target& target) {
        return example::mesh_frame::point(target.surface.frame,target.surface.position);
    }
    static Edit move_to(const Target& target,vng::Vec3 destination) {
        const auto point=example::mesh_frame::point(target.inverse,destination);
        const auto& surface=target.surface;
        double radius{};
        for(unsigned c=0;c<3;++c)radius+=double(target.radial[c])*(point[c]-surface.center[c]);
        radius=std::clamp(radius,double(surface.radial_range->x),double(surface.radial_range->y));
        Edit edit;
        for(unsigned c=0;c<3;++c)edit.local_position[c]=surface.center[c]+vng::f32(radius)*target.radial[c];
        return edit;
    }
};
}
