#pragma once
#include <vng/ui/ui.hpp>
#include <vng/render/renderer.hpp>

namespace vng::render {
struct UiRenderStats {
    u32 boxes{}, glyphs{}, draw_calls{}, glyph_uploads{};
    u32 images{}, image_uploads{}, cached_images{};
    u32 text_vertex_uploads{}; // all labels share one upload, regardless of painter-order runs
};
struct MakeUiRenderer {
    template <class Device>
    [[nodiscard]] auto
    operator()(Device& device) const -> decltype(make_backend_ui_renderer(device)) {
        return make_backend_ui_renderer(device);
    }
};
inline constexpr MakeUiRenderer make_ui_renderer{};
} // namespace vng::render
