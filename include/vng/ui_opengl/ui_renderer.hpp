#pragma once
#include <vng/ui/ui_renderer.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/renderer.hpp>
#include <memory>

namespace vng::opengl {
class UiRenderer final : public Renderer<ui::DrawList> {
public:
    [[nodiscard]] static std::expected<UiRenderer, Diagnostic> create(Device&);
    UiRenderer(UiRenderer&&) noexcept;
    UiRenderer& operator=(UiRenderer&&) noexcept;
    ~UiRenderer();
    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;
    [[nodiscard]] std::expected<void, Diagnostic> render(Frame&, const ui::Screen&);
    [[nodiscard]] std::expected<void, Diagnostic> render(Frame&, const ui::DrawList&);
    [[nodiscard]] std::expected<void, Diagnostic> render(Frame&, const render::RenderView&,
                                                         std::span<const ui::DrawList>);
    [[nodiscard]] render::UiRenderStats stats() const noexcept;
    [[nodiscard]] std::expected<void, Diagnostic> clear_cache(const Device&);

private:
    struct Impl;
    explicit UiRenderer(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] inline std::expected<UiRenderer, Diagnostic>
make_backend_ui_renderer(Device& device) {
    return UiRenderer::create(device);
}
} // namespace vng::opengl
