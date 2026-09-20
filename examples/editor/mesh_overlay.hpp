#pragma once
#include "mesh_tools.hpp"
#include <vng/opengl/frame.hpp>
#include <memory>

namespace editor_example {
// OpenGL realization of the local editing overlay. Selection/topology remain
// backend-neutral in MeshTools; no overlay geometry or readback crosses IPC.
class MeshOverlay {
public:
    struct Stats { vng::u32 draws{}, position_uploads{}, index_uploads{}, topology_uploads{}, selection_uploads{}; };
    static std::expected<MeshOverlay,vng::opengl::Diagnostic> create(vng::opengl::Device&);
    MeshOverlay(MeshOverlay&&) noexcept;
    MeshOverlay& operator=(MeshOverlay&&) noexcept;
    ~MeshOverlay();
    std::expected<void,vng::opengl::Diagnostic> render(vng::opengl::Frame&, const State&,
        const MeshTools&, vng::ui::Rect, vng::Vec2 logical_size, vng::Extent2D camera_extent,
        const vng::gfx::Camera&);
    [[nodiscard]] Stats stats() const;
private:
    struct Impl;
    explicit MeshOverlay(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
