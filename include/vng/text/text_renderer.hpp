#pragma once

#include <optional>
#include <string_view>
#include <utility>

#include <vng/core/types.hpp>
#include <vng/text/font.hpp>

namespace vng::render {

struct TextClip final {
    f32 x{}, y{}, width{}, height{};
};

// Text is borrowed for the duration of render(). Font is a cheap owning
// handle. Position and size are framebuffer pixels, with +Y pointing down.
// Position is the top-left of the first line box (not its baseline).
struct TextDraw final {
    std::string_view text;
    Vec2 position{};
    u32 size{24};
    Vec4 color{1, 1, 1, 1}; // linear RGB, straight opacity
    text::Font font{};      // empty selects the renderer's default font
    std::optional<TextClip> clip{};
};

struct TextRendererOptions final {
    u32 atlas_size{1024};
    u32 max_atlas_pages{16};
    u32 max_cached_layouts{256};
    u32 max_glyphs_per_render{65536};
};

struct TextRenderStats final {
    u32 glyphs{};        // visible glyph quads submitted
    u32 draw_calls{};    // adjacent atlas-page runs, in ticket order
    u32 glyph_uploads{}; // newly rasterized nonempty glyphs this call
    u32 layout_hits{};
    u32 atlas_pages{};    // total resident pages
    u32 vertex_uploads{}; // one geometry upload for a nonempty submission
};

struct MakeTextRenderer final {
    template <class Device>
    [[nodiscard]] auto operator()(Device& device, text::Font default_font = {},
                                  TextRendererOptions options = {}) const
        -> decltype(make_backend_text_renderer(device, std::move(default_font), options)) {
        return make_backend_text_renderer(device, std::move(default_font), options);
    }
};
inline constexpr MakeTextRenderer make_text_renderer{};

struct TextRendererBuilderFactory final {
    template <class Device>
    [[nodiscard]] auto
    operator()(Device& device) const -> decltype(make_backend_text_renderer_builder(device)) {
        return make_backend_text_renderer_builder(device);
    }
};
inline constexpr TextRendererBuilderFactory text_renderer_builder{};

} // namespace vng::render
