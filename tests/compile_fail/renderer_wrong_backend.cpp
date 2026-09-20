#include <vng/opengl/renderer.hpp>

struct Draw {};
struct OtherDevice;
struct OtherFrame;
struct OtherBackend {
    using device_type = OtherDevice;
    using frame_type = OtherFrame;
};
struct OtherFrame {
    using backend_type = OtherBackend;
    bool active() const noexcept { return true; }
    vng::Extent2D extent() const noexcept { return {1,1}; }
    void end() {}
};
class OpenGLRenderer : public vng::opengl::Renderer<Draw> {
public:
    // Deliberately broad: callability alone is not backend compatibility.
    template<class F>
    void render(F&, const vng::render::RenderView&, std::span<const Draw>) {}
};
int main() {
    OpenGLRenderer renderer;
    OtherFrame frame;
    vng::render::render_one(renderer, frame,
        vng::render::RenderView::without_camera({1,1}), Draw{});
}
