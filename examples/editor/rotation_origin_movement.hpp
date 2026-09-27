#pragma once
#include "transform_pivot.hpp"

namespace editor_example {
// A private editor cursor, not a scene instance or a document command.
struct RotationOriginMovement {
    using Target = TransformPivot;
    struct Edit { vng::Vec3 point; };
    static vng::Vec3 world_position(const Target& target) { return target.point; }
    static Edit move_to(const Target&,vng::Vec3 destination) { return {destination}; }
};
}
