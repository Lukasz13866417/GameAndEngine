#pragma once
#include <vng/core/types.hpp>

namespace editor_example {
// Small evaluated snapshot, never a copy of an instance's mesh or boundary.
// Moving a selection places its primary absolutely; EditingSession preserves
// the other selected instances' gesture-start offsets.
struct InstanceMovement {
    struct Target { vng::u32 object; vng::Vec3 position; };
    struct Edit { vng::u32 object; vng::Vec3 position; };
    static vng::Vec3 world_position(const Target& target) { return target.position; }
    static Edit move_to(const Target& target,vng::Vec3 destination) { return {target.object,destination}; }
};
}
