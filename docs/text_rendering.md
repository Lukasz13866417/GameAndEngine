# Text rendering

Load a font, create a text renderer, and submit strings. The renderer owns its
shaders, glyph textures, caches, and reusable vertex buffer. Application code
does not construct glyph meshes or shaders.

```cpp
#include <array>
#include <vng/opengl/text_renderer.hpp>
#include <vng/render/text_renderer.hpp>
#include <vng/text/font.hpp>

auto font = vng::text::Font::load("examples/assets/fonts/DejaVuSans.ttf");
auto text = vng::render::make_text_renderer(device, *font);

// After beginning a frame:
const std::array labels{
    vng::render::TextDraw{
        .text = "Hello, world!",
        .position = {24, 24},
        .size = 32,
    },
    vng::render::TextDraw{
        .text = "Score: 1200",
        .position = {24, 72},
        .size = 20,
        .color = {1.0F, 0.8F, 0.2F, 1.0F},
    },
};
auto result = text->render(frame, labels);
```

Snippets omit error handling for readability. Loading, creating, measuring,
rendering, and cache reset return `std::expected`; check each result before
using its value. [The text demo](../examples/text.cpp) includes error handling.

`make_text_renderer` selects the backend through the device type. For an
OpenGL device, it returns `std::expected<opengl::TextRenderer,
opengl::Diagnostic>`. That concrete class derives from
`opengl::Renderer<render::TextDraw>` and satisfies the `RendererFor`
contract. The neutral ticket, font, and factory headers contain no OpenGL
types. Include the backend renderer header where the factory is invoked.

## Tickets and coordinates

Each `TextDraw` supplies a borrowed UTF-8 string, a position, a pixel size,
and a color. Its optional `font` overrides the default passed to the factory:

```cpp
auto alternate = vng::text::Font::load("another-font.ttf");
vng::render::TextDraw label{
    .text = "A different font",
    .position = {24, 110},
    .size = 28,
    .font = *alternate,
    .clip = vng::render::TextClip{24, 110, 240, 40},
};
text->render(frame, label);
```

The default font is optional if every ticket supplies one. Missing both is an
error. `Font` is a cheap shared owning handle; loading reads and retains the
file bytes. Copies share a stable cache identity and remain usable after the
original handle or file disappears. The ticket's string must remain alive
for the duration of `render()`; cached layouts own their string keys.

Coordinates use framebuffer pixels, with `(0, 0)` at the target's top-left
and positive Y downward. `position` is the top-left of the first line box,
not the baseline or visible ink bounds. `size` is an integer pixel size in
`[1, 4096]`; it is independent of the camera. Account for the window's
framebuffer scale when choosing a size for a high-DPI display.

Colors use linear RGB and straight opacity; all four components must be finite
and in `[0, 1]`. The shader converts coverage and opacity to premultiplied
output, and the renderer selects premultiplied-alpha blending. Output encoding
continues to belong to the frame target.

An optional clip rectangle uses the same absolute framebuffer coordinates.
Its dimensions must be finite and nonnegative. The renderer clips glyph
geometry and adjusts texture coordinates on the CPU, so different clip
rectangles can remain in the same draw call. The target bounds also clip text.

## Measurement and frame integration

```cpp
auto metrics = text->measure(label);
// metrics->width: maximum line advance
// metrics->height: total line-box height
// metrics->baseline: first baseline, measured downward from label.position.y
// metrics->line_height and metrics->line_count describe the line boxes
```

Measurement ignores clipping and returns logical layout dimensions rather than
the bounds of visible glyph pixels. It does not require a current GL context
or populate the renderer's layout cache. For CPU-only layout, use
`font.measure(utf8, size)` or `font.layout(utf8, size)` directly. Empty text has
zero width, height, and line count; the selected font's baseline and line
height are still available.

Render text after world geometry when it should appear as an overlay. Text
disables depth testing, depth writing, and culling, and does not clear the
target. It borrows the frame's shared command context, leaving existing parent
context and graphics-state handles valid. There is no implicit state restoration:
if drawing more geometry afterward, select its program and desired settings
through the existing handle. The overload accepting `RenderView` checks that its extent matches the
frame; the camera itself does not affect screen-space text.

The renderer is move-only. Creation, rendering, cache reset, and destruction
of its GPU resources require the owning OpenGL context to be current on its
owning thread. CPU `Font` copies synchronize their shared face access;
`TextRenderer` itself follows the device's single-threaded rendering rules.

## Shaping, caching, and draw calls

FreeType loads outlines and rasterizes grayscale coverage. HarfBuzz shapes
UTF-8 into glyph indices, advances, offsets, and source byte clusters. Kerning,
ligatures, combining marks, and contextual script forms consequently come
from the font's shaping data. Size changes update both libraries together.

The layout cache uses `(font identity, pixel size, UTF-8 bytes)` as its key;
changing color, position, or clipping reuses the layout. Cached entries use
least-recently-used eviction. Glyphs use `(font identity, pixel size, glyph
index)` and are rasterized and uploaded on first use. Spaces and other empty
glyphs need no texture area or submitted quad.

Nonempty glyphs are packed into lazily allocated single-channel `R8` atlas
pages, with transparent padding and linear filtering. Different fonts and
sizes can share a page. The renderer generates six vertices per visible
glyph and uploads them to one reusable buffer, growing its allocation when
needed. Its vertex and fragment shaders are built internally through the
engine's typed DSL and compiled once at renderer creation. The fragment shader
samples the atlas's coverage channel and applies the ticket color.

Submissions preserve ticket and glyph order so overlapping translucent labels
compose predictably. Adjacent glyphs using the same atlas page share a draw
call, including across ticket, font, size, color, and clip changes. The renderer
does not reorder labels to group nonadjacent page uses. Typical text that fits
on one page takes one draw call for the entire submitted span.

After a successful `render()`, `stats()` exposes:

| Field | Meaning |
| --- | --- |
| `glyphs` | Visible glyph quads submitted in that call. |
| `draw_calls` | Adjacent atlas-page runs actually drawn. |
| `glyph_uploads` | Newly rasterized nonempty glyphs uploaded in that call. |
| `layout_hits` | Tickets that found an existing shaped layout. |
| `atlas_pages` | Total resident atlas pages. |
| `vertex_uploads` | Zero for empty geometry; one otherwise, independent of atlas-page runs. |

Glyph caching currently happens before geometry clipping; a clipped label can
warm its glyph cache even when it submits no visible quads. Repeated rendering
with resident glyphs needs no new glyph uploads. Empty strings, fully
transparent labels, and zero-area clip rectangles skip shaping and rasterization.
Warm layout lookup borrows the submitted string; it only allocates an owned
key when inserting a new cached layout.

## Cache limits and supported text

The factory optionally accepts `render::TextRendererOptions`:

```cpp
auto text = vng::render::make_text_renderer(device, *font, {
    .atlas_size = 1024,
    .max_atlas_pages = 16,
    .max_cached_layouts = 256,
    .max_glyphs_per_render = 65536,
});
```

These are the defaults. A `1024 x 1024` `R8` page stores 1 MiB of coverage, so
the default atlas limit is 16 MiB, excluding other GPU and CPU allocations.
Pages have a fixed size and no automatic eviction or repacking. A glyph that
does not fit on one page, or a full atlas, produces a diagnostic. Increase the
appropriate limit or call `text->clear_cache(device)` to discard resident
glyphs, layouts, and pages. The reusable vertex buffer and compiled program
are retained. Setting `max_cached_layouts` to zero disables layout caching.
The glyph-per-render limit counts shaped glyphs, including spaces and clipped
glyphs, rather than only visible quads.

Atlas size must be in `[16, 16384]`, subject also to device texture limits;
page count is in `[1, 256]`, layout count in `[0, 65536]`, and glyph submission
limit in `[1, 1000000]`. The atlas memory bound does not bound every CPU cache
allocation: layouts are limited by entry count, and glyph metadata remains
until explicit cache reset.

The current text implementation supports scalable outline fonts, strict UTF-8,
explicit newlines (`\n`, `\r`, and `\r\n`), and four-space tab stops. Each
tab-separated line segment is shaped as one run with inferred script and
direction. A single Arabic run is supported; complete mixed-direction Unicode
paragraph layout is not. There is no automatic font fallback, wrapping,
alignment, rich-text markup, color emoji, editable text widget, or world-space
text mode. Missing characters use the selected font's missing-glyph symbol.
Font files are limited to 64 MiB and individual input strings to 16 MiB.

## Build and example

`VNG_BUILD_TEXT` defaults to `ON`. The `vng::text` target provides CPU font
loading, shaping, measurement, and rasterization, with no graphics or window
dependency. `vng::text_opengl` adds the concrete renderer when the OpenGL
backend is enabled. Its OpenGL dependencies do not include GLFW; the demo
uses the separate GLFW integration for its window.

FreeType and HarfBuzz are used from installed packages when available. Pinned
fallbacks are FreeType 2.13.3 and HarfBuzz 11.2.1. Neither dependency's types
appear in the public font API. Set `VNG_BUILD_TEXT=OFF` to omit both text
targets and their dependencies.

```sh
cmake -S . -B build
cmake --build build --target vng_text_demo
./build/vng_text_demo
./build/vng_text_demo --once
```

The demo uses the bundled DejaVu Sans font; its license is included beside the
font in `examples/assets/fonts/`. The CPU `vng_text_tests` target can also be
built with both `VNG_BUILD_OPENGL` and `VNG_BUILD_WINDOW_GLFW` disabled.
