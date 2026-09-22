#pragma once
#include "mesh_overlay.hpp"
#include <vng/input/input.hpp>
#include <vng/glfw_opengl/glfw_opengl.hpp>
#include <vng/resources/diagnostic.hpp>
#include <memory>
#include <string>

namespace editor_example {
// Optional presentation child of the editor, NOT another document or worker.
// Owns its context and GPU resources; all operations restore the UI context.
class ViewportWindow {
public:
    explicit ViewportWindow(vng::glfw_opengl::Window& controls);
    ~ViewportWindow();
    ViewportWindow(const ViewportWindow&) = delete;
    ViewportWindow& operator=(const ViewportWindow&) = delete;
    std::expected<void, std::string> open(bool visible);
    void close();
    bool opened() const;
    bool closing() const;
    vng::input::Frame take_input();
    // False when a minimized/resizing window skipped presentation.
    std::expected<bool, std::string> present(const vng::ui::DrawList&, const vng::ui::DrawList& foreground, vng::window::VSync,
        const State&, const MeshTools&, vng::ui::Rect, vng::Extent2D camera_extent,
        const vng::gfx::Camera&, bool show_mesh);
    vng::resources::Result<vng::gfx::ImageData> capture();
private:
    struct Surface;
    vng::glfw_opengl::Window& controls_;
    std::unique_ptr<Surface> surface_;
};
}
