#pragma once
#include "project.hpp"

namespace editor_example {
enum class PivotMode { selection, individual, custom };
struct TransformPivot {
    PivotMode mode{PivotMode::selection};
    vng::Vec3 point{}; // World coordinates for custom; never authored scene data.
    friend bool operator==(const TransformPivot&,const TransformPivot&)=default;
};
struct InstanceCenter {
    vng::u32 object;
    InstanceTransform transform;
    vng::Vec3 local,world;
};
// Equal weight per instance. Mesh centers average stored vertices; region
// centers average boundary points. No physics-mass or uniform-density claim.
[[nodiscard]] std::vector<InstanceCenter> instance_centers(const State&,vng::u32 primary,std::span<const vng::u32> selection);
[[nodiscard]] vng::Vec3 selection_center(std::span<const InstanceCenter>);
}
