#include "viewport_window.hpp"
#include "../support/glfw_opengl_session.hpp"
#include "../support/presentation.hpp"
#include <vng/ui_opengl/ui_renderer.hpp>

namespace editor_example {
using namespace vng;
namespace {
// Also covers early returns and exception unwinding during resource creation.
struct RestoreContext {
    glfw_opengl::Window& window;
    ~RestoreContext() { (void)window.make_current(); }
};
}
struct ViewportWindow::Surface {
    example::GlfwOpenGLSession session;
    opengl::UiRenderer renderer;
    MeshOverlay overlay;
    example::DisplaySurface display;
};
ViewportWindow::ViewportWindow(glfw_opengl::Window& controls) : controls_(controls) {}
ViewportWindow::~ViewportWindow() { close(); }
bool ViewportWindow::opened() const { return bool(surface_); }
bool ViewportWindow::closing() const { return surface_ && surface_->session.window().should_close(); }
void ViewportWindow::close() {
    if (!surface_) return;
    RestoreContext restore{controls_};
    (void)surface_->session.window().make_current();
    surface_.reset(); // GL objects die before their own context/window.
}
std::expected<void, std::string> ViewportWindow::open(bool visible) {
    if (surface_) return {};
    RestoreContext restore{controls_};
    auto session = example::GlfwOpenGLSession::create(
        {.width=1200, .height=900, .title="Vibe / Editable viewport (close to dock)",
         .visible=visible, .resizable=true},
        {.debug=true, .samples=0, .default_framebuffer_encoding=render::ColorEncoding::linear},
        {.vsync=window::VSync::off});
    if (!session) return std::unexpected(std::visit([](const auto& e){return e.message;}, session.error()));
    auto renderer = render::make_ui_renderer(session->device());
    if (!renderer) return std::unexpected(renderer.error().message);
    auto overlay = MeshOverlay::create(session->device());
    if (!overlay) return std::unexpected(overlay.error().message);
    auto display = example::DisplaySurface::create(session->device(),session->window().framebuffer_extent(),true);
    if (!display) return std::unexpected(display.error().message);
    surface_ = std::make_unique<Surface>(std::move(*session),std::move(*renderer),std::move(*overlay),std::move(*display));
    return {};
}
input::Frame ViewportWindow::take_input() { return surface_->session.window().take_input(); }
std::expected<bool, std::string> ViewportWindow::present(const ui::DrawList& list,const ui::DrawList& foreground, window::VSync vsync,
    const State& state, const MeshTools& tools, ui::Rect viewport, Extent2D camera_extent,
    const gfx::Camera& camera, bool show_mesh) {
    auto& s=*surface_;
    auto& window=s.session.window();
    // A resize arriving after input/layout is handled by the next UI tick.
    if (window.framebuffer_extent()!=list.framebuffer || !list.framebuffer.width || !list.framebuffer.height) return false;
    RestoreContext restore{controls_};
    if (auto r=window.make_current();!r) return std::unexpected(r.error().message);
    if (window.vsync()!=vsync)
        if (auto r=window.set_vsync(vsync);!r) return std::unexpected(r.error().message);
    auto& device=s.session.device();
    if (auto r=s.display.resize(device,list.framebuffer);!r) return std::unexpected(r.error().message);
    auto frame=render::begin_frame(device,s.display.target(),
        {.extent=list.framebuffer,.color_encoding=render::ColorEncoding::srgb,
         .clear_color=std::array<f32,4>{.007F,.01F,.019F,1},.clear_depth={}});
    if (!frame) return std::unexpected(frame.error().message);
    if (auto r=s.renderer.render(*frame,list);!r) return std::unexpected(r.error().message);
    if (show_mesh)
        if (auto r=s.overlay.render(*frame,state,tools,viewport,list.logical_size,camera_extent,camera);!r)
            return std::unexpected(r.error().message);
    if(!foreground.commands.empty())
        if(auto r=s.renderer.render(*frame,foreground);!r)return std::unexpected(r.error().message);
    if (auto r=frame->end();!r) return std::unexpected(r.error().message);
    if (auto r=s.display.copy_to_window(device);!r) return std::unexpected(r.error().message);
    if (auto r=window.present();!r) return std::unexpected(r.error().message);
    return true;
}
resources::Result<gfx::ImageData> ViewportWindow::capture() {
    RestoreContext restore{controls_};
    auto& s=*surface_;
    if (auto r=s.session.window().make_current();!r) {
        resources::Diagnostic error; error.message=r.error().message;
        return std::unexpected(std::move(error));
    }
    // The offscreen presentation surface remains valid after the buffer swap.
    if (auto r=s.display.copy_to_window(s.session.device());!r) return std::unexpected(r.error());
    return example::capture_screenshot(s.session.device(),s.session.window().framebuffer_extent());
}
}
