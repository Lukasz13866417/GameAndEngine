#pragma once

#include <vng/core/types.hpp>

namespace vng::render {

// Ticket accepted by the optional single-mesh renderer convenience. The
// renderer owns the mesh; each ticket requests another (possibly instanced)
// draw of it. Applications with per-object data can instead define their own
// ticket and renderer policy on top of Renderer<Ticket>.
struct MeshDraw final {
    u32 instance_count{1};

    friend constexpr bool operator==(const MeshDraw&, const MeshDraw&) = default;
};

} // namespace vng::render
