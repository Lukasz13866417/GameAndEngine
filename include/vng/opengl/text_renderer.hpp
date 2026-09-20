#pragma once

#include <expected>
#include <memory>
#include <span>

#include <vng/opengl/diagnostic.hpp>
#include <vng/opengl/renderer.hpp>
#include <vng/render/text_renderer.hpp>
#include <vng/resources/resources.hpp>

namespace vng::opengl {
class Device;
class Frame;
class Commands;
class UiRenderer;
class TextRendererBuilder;

// Owns DSL shaders, atlas pages, shaped-text/glyph caches and a reusable
// vertex buffer. Rendering starts a new frame command stream, like other
// renderer policies. Screen-space text is independent of the camera.
class TextRenderer final : public Renderer<render::TextDraw> {
public:
    using FontProvider = resources::Provider<text::Font, Device>;
    [[nodiscard]] static std::expected<TextRenderer, Diagnostic>
    create(Device& device, text::Font default_font = {}, render::TextRendererOptions options = {});

    TextRenderer(TextRenderer&&) noexcept;
    TextRenderer& operator=(TextRenderer&&) noexcept;
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    [[nodiscard]] std::expected<void, Diagnostic> render(Frame& frame,
                                                         std::span<const render::TextDraw> tickets);
    [[nodiscard]] std::expected<void, Diagnostic>
    render(Frame& frame, const render::RenderView& view, std::span<const render::TextDraw> tickets);
    [[nodiscard]] std::expected<void, Diagnostic> render(Frame& frame,
                                                         const render::TextDraw& ticket);

    [[nodiscard]] std::expected<text::TextMetrics, Diagnostic>
    measure(const render::TextDraw& ticket) const;
    [[nodiscard]] render::TextRenderStats stats() const noexcept;
    // Explicit cache reset for font/theme changes or reclaiming atlas memory.
    [[nodiscard]] std::expected<void, Diagnostic> clear_cache(const Device& device);

    [[nodiscard]] resources::Result<void> reload_font(Device& device);
    [[nodiscard]] resources::Result<void> reload_font(Device& device, FontProvider provider);
    [[nodiscard]] resources::Result<void> replace_font(Device& device, text::Font font);
    template <class P>
        requires resources::ProviderFor<P, text::Font, Device>
    [[nodiscard]] resources::Result<void> replace_font(Device& device,
                                                       resources::Provided<text::Font, P> font) {
        return install_font(device, std::move(font.value), FontProvider{std::move(font.provider)});
    }
    [[nodiscard]] resources::Result<resources::ReloadReport> reload(Device& device);

private:
    friend class TextRendererBuilder;
    friend class UiRenderer;
    // UI painter-order runs share one prepared upload and command stream.
    // These remain internal: ordinary callers only submit render() tickets.
    [[nodiscard]] std::expected<void, Diagnostic>
    prepare(Frame& frame, std::span<const render::TextDraw> tickets);
    [[nodiscard]] std::expected<void, Diagnostic>
    draw_prepared(const Device& device, Commands& commands, u32 first_ticket, u32 ticket_count);
    [[nodiscard]] resources::Result<void> validate_update(const Device& device) const;
    [[nodiscard]] resources::Result<void> install_font(Device& device, text::Font font,
                                                       FontProvider provider);
    struct Impl;
    explicit TextRenderer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    bool providing_{};
};

class TextRendererBuilder final {
public:
    explicit TextRendererBuilder(Device& device) : device_(&device) {}
    TextRendererBuilder& font(TextRenderer::FontProvider provider) {
        provider_ = std::move(provider);
        font_ = {};
        return *this;
    }
    TextRendererBuilder& font(text::Font font) {
        font_ = std::move(font);
        provider_ = {};
        return *this;
    }
    TextRendererBuilder& options(render::TextRendererOptions options) {
        options_ = options;
        return *this;
    }
    [[nodiscard]] resources::Result<TextRenderer> build() const;

private:
    Device* device_;
    TextRenderer::FontProvider provider_;
    text::Font font_;
    render::TextRendererOptions options_;
};

[[nodiscard]] inline TextRendererBuilder make_backend_text_renderer_builder(Device& device) {
    return TextRendererBuilder{device};
}

[[nodiscard]] std::expected<TextRenderer, Diagnostic>
make_backend_text_renderer(Device& device, text::Font default_font,
                           render::TextRendererOptions options);

} // namespace vng::opengl
