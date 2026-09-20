# UI

Persistent widgets, polling-based interaction, a backend-neutral draw list,
and a renderer that owns its shaders and reuses the text renderer. Widget
creation does not require a graphics context or user-supplied IDs.

## Run the example

```sh
cmake -S . -B build
cmake --build build --target vng_ui_demo
./build/vng_ui_demo
./build/vng_ui_demo --screenshot /tmp/ui-new.png
```

The demo has buttons, checkboxes, a typed dropdown, a single-line field,
a multiline area, scrolling, and labels that report polled state. Screenshot
paths must be new. `--once` closes after one frame; omit it for interaction.
This is a standalone controls demo, not an overlay added to the solar flyby.

UI requires text support and installed ICU development files (version 67+;
`libicu-dev` on Debian/Ubuntu). `VNG_BUILD_UI=OFF` omits UI/ICU while keeping
text; `VNG_BUILD_TEXT=OFF` omits both. The visible demo additionally needs
OpenGL and GLFW.

## Small application API

```cpp
#include <vng/opengl/ui_renderer.hpp>

namespace ui = vng::ui;

auto font = vng::text::Font::load("examples/assets/fonts/DejaVuSans.ttf");
ui::Screen screen{ui::dark_theme(*font)};
auto renderer = vng::render::make_ui_renderer(device);

auto panel = screen.column().position({24, 24}).width(320);
auto pause = panel.button("Pause");
auto bloom = panel.checkbox("Bloom").value(true);
auto name = panel.text_input("Ship name").value("Kestrel");
auto notes = panel.text_area("Notes").height(120);

// Each frame: poll the window, then update UI exactly once.
window.poll_events();
auto input = ui::update(screen, window, dt);

if (pause.clicked()) paused = !paused;
if (pause.isPressed()) { /* held by pointer or keyboard */ }
if (panel.isHovered()) { /* includes its hovered descendants */ }
if (auto enabled = bloom.changedValue()) bloom_enabled = *enabled;
if (auto text = name.changedText()) ship_name = *text;
if (auto text = notes.submittedText()) save_notes(*text);

// Use the filtered result for game input, not the window's raw held keys.
if (input->keyDown(vng::input::Key::w)) move_forward();
for (const auto& event : input->unhandled()) game_input(event);

// Draw on the final output frame, after scene postprocessing/tone mapping.
renderer->render(frame, screen);
```

Snippets omit error handling; [the demo](../examples/ui.cpp) checks results.
Font loading, renderer creation, UI update/draw-list generation, and rendering
return `std::expected`. Invalid widget construction/setters throw standard
exceptions (for example invalid UTF-8, negative dimensions or unknown choices).
The UI does not run callbacks during traversal: update, inspect state, change
application state, and render. Ordinary `if` statements are the behavior API.

### State versus events

| Query | Meaning |
| --- | --- |
| `isHovered()` | The hit-tested widget or an ancestor of it; respects clipping/occlusion. |
| `isPressed()` | Pointer capture or a held activation key, not a completed click. |
| `isFocused()` | Keyboard focus. |
| `clicked()` | Button activated during this update (release inside, or keyboard activation). |
| `value()` / `getText()` | Current value, regardless of whether it changed. |
| `changedValue()` / `changedText()` | Optional value after user edits during this update. |
| `submittedText()` | Optional value on Enter; Ctrl+Enter for multiline areas. |

Event queries do not consume anything. They can be read by multiple systems,
and reset at the next `Screen::update()`, not during rendering. Several edits
in one update coalesce to the final value; this is not a per-keystroke log.
An engaged `optional<bool>{false}` and an engaged empty text are still changes.
`getText()` is a `string_view`, not an optional; test `changedText()` when asking
whether something happened. Copy a returned text view if it must outlive the
next edit/removal/screen destruction.

Pointer capture keeps a dragged-out button pressed until release, without
clicking it outside. A popup intercepts outside clicks to dismiss itself without
also activating a control underneath. Disabled/hidden/removed ancestors cancel
their descendants' capture and focus. Losing window focus also cancels input.

`UpdateResult::unhandled()` preserves unconsumed event order. Held-key masking
also remembers consumed key presses until release, preventing a focus change
from leaking the same key to the game. `capturesKeyboard` and `capturesPointer`
describe current UI capture/hit state. Consume unhandled events for precise
per-event routing. Don't also feed the same raw events to game controls.

## Layout, values, and ownership

```cpp
auto actions = panel.row().padding(0).gap(8);
auto restart = actions.button("Restart").width(120);
auto hide = actions.button("Hide").width(120);

enum class Quality { low, medium, high };
auto quality = panel.dropdown<Quality>("Quality", {
    {Quality::low, "Low"},
    {Quality::medium, "Medium"},
    {Quality::high, "High"},
}).value(Quality::high);

auto log = screen.column().position({400, 24}).width(300).height(220);
log.label("Scrolling content");
// Overflowing columns get a vertical scrollbar automatically.
log.scrollbar(ui::ScrollBar::always); // reserve a gutter even when content fits
// ui::ScrollBar::hidden keeps wheel scrolling without a visible bar.
log.scroll(120);                     // offset in logical pixels
auto offset = log.scroll();
auto limit = log.scroll_limit();

name.focus();
restart.enabled(false);
panel.visible(false);
// panel.remove(); // destroys the subtree's content and invalidates its handles
```

Coordinates and theme dimensions are logical window pixels, top-left origin,
positive Y downward. The renderer scales to framebuffer pixels automatically.

`Splitter` is a thin draggable divider, not a value slider. It reports relative
movement in logical pixels; the owning layout decides which panes to resize:

```cpp
auto upper = panel.column().height(200);
auto divider = panel.splitter("Resize panes", ui::SplitAxis::y)
                    .range(60, 500).value(200);
auto lower = panel.column().height(300);
// After screen.update(...):
if (auto height = divider.changedValue()) {
    upper.height(*height);
    lower.height(500 - *height);
}
```

Use `SplitAxis::x` for left/right panes. Pressing the divider does not jump its
value. Capture survives its own movement and release outside the widget; Escape
or focus loss restores the drag's starting value. Arrow keys adjust a focused
divider by one logical pixel (Shift: ten). `editStarted()`, `editCommitted()` and
`editCancelled()` are available for layout owners. The label is exposed to
inspection/accessibility tools, not painted on the grip. The control has no
renderer/backend or scene dependency beyond the ordinary neutral UI layer.

Hit testing uses the same logical bounds as drawing. Columns stack vertically,
rows horizontally; padding and gaps come from the theme unless overridden.
Column children fill the available width by default; row children use natural
widths. Unspecified container heights fit children. Root children use explicit
positions and are drawn in insertion order; later siblings are in front.
`position()` offsets a child relative to its normal flow position, not the screen.

Children are clipped to their containers. Dropdown menus escape parent clipping
and widen to fit their options independently of the closed control, bounded by
the viewport; long menus scroll. Tab/Shift+Tab traverse
interactive controls in tree order and reveal them in scroll containers.
Scrollbars have a slim themed track and a proportional thumb, distinct from
value sliders. Drag the thumb, click the track to seek, or use the wheel. Nested
columns scroll independently; a wheel event at a child's edge can scroll its
parent. The gutter reserves space instead of covering content. Escape cancels
a thumb drag, and release outside the panel still ends it. Scrollbar width,
gap, minimum thumb size, and colors live in `ThemeValues`.
Captured slider and scrollbar drags use the latest contiguous mouse-motion sample
in each input update. This avoids repeated layout for high-polling-rate mice;
release, keyboard and focus events retain their original order, and viewport
motion is passed through unchanged. Reapplying a slider's unchanged `range()`
preserves its active drag and change notifications; changing the range cancels it.
`Screen::inspect()` exposes each bar as a stable virtual child of its container,
with track/thumb rectangles and pixel offset/range for input-driven testing.
This first layout system has no flex weights, automatic responsive breakpoints,
grid, or implicit horizontal scrolling.

The `Screen` owns nodes and strings. Handles are cheap, copyable, weak references;
moving a screen preserves them. Removed node slots are recycled, but allocation
identities and inspection IDs are never reused: old handles cannot refer to a
replacement widget. `valid()` and
state polling return false for expired handles; value access/mutation throws.
Only the UI thread may update/read these objects. A screen holds at most 262,144
live nodes (including the root), nesting is limited to 128 levels, and
each text value is limited to 64 KiB. Reuse retained widgets for dynamic content
rather than recreating the tree each frame. Removal releases widget resources;
frame updates and inspection traverse live nodes, not historical allocations.

`destination.adopt(group)` moves an existing container group to the end of
another container in the same screen. Its handles, values and inspection IDs
survive; active pointer capture, keyboard focus and dropdown popups in that group
are cancelled on moving. Cycles, cross-screen moves and excessive nesting are
rejected before changing the tree. The editor uses this to move toolbar groups
into overflow menus when space is limited, without duplicating controls or
rebuilding their state each frame.

## Themes

The built-in theme supplies sensible defaults. A custom theme implements one
getter; extra application-specific values can be ordinary subclass fields.

```cpp
class CockpitTheme final : public ui::Theme {
public:
    explicit CockpitTheme(vng::text::Font font)
        : base_(ui::dark_theme_values(std::move(font))) {
        base_.accent = {0.1F, 0.65F, 0.8F, 1.0F};
        base_.radius = 3;
    }
    const ui::ThemeValues& values() const noexcept override { return base_; }
    vng::Vec4 danger{0.8F, 0.05F, 0.02F, 1.0F};
private:
    ui::ThemeValues base_;
};

screen.set_theme(std::make_shared<CockpitTheme>(*font));
```

Themes are shared as `shared_ptr<const Theme>`. Treat one as immutable after
installation; use `set_theme()` to invalidate layout/font measurements when
changing it. `ThemeValues` includes the font, font size, spacing, control height,
rounding, border width and standard colors. Colors are linear RGB with straight
opacity; the GPU renderer handles premultiplication and the frame handles output
encoding. No shader subclass or duplicate per-widget style declaration is needed.

## Backend boundary and rendering

```text
vng_ui                 -> vng_text -> vng_core
   private dependency  -> ICU (Unicode grapheme segmentation)

vng_ui_opengl          -> vng_ui + vng_text_opengl
vng_text_opengl        -> vng_text + vng_opengl

vng_window_glfw        -> vng_core + GLFW (no UI/OpenGL dependency)
vng_glfw_opengl        -> vng_window_glfw + vng_opengl
```

`vng_ui` contains no OpenGL or GLFW headers and can be tested headlessly.
`Screen::update(input::Frame, dt, Clipboard*)` is the core entry point. The
optional `ui::update(screen, window, dt)` bridge adapts `take_input()` and clipboard
methods without tying UI to a window implementation. Polling windows is global
within GLFW, but `take_input()` drains only that window's ordered queue. Overflow
is explicit and cancels focus/capture rather than silently dropping a release.

`render::make_ui_renderer(device)` selects the concrete backend through the
device type. `opengl::UiRenderer` is a move-only `opengl::Renderer<ui::DrawList>`; it owns
the DSL-generated rounded-rectangle program, buffer and VAO, and a TextRenderer.
There is no per-widget backend renderer subclass or backend field on a button.

For inspection, custom drawing or an alternate backend:

```cpp
auto list = screen.draw_list();
auto bounds = pause.bounds();
renderer->render(frame, *list);
auto stats = renderer->stats(); // boxes, glyphs, draw_calls, glyph_uploads
```

Draw lists own their strings and font handles and remain usable after the screen
changes or is destroyed. They contain ordered `BoxDraw`/`TextDraw` commands with
explicit clips, making geometry, text and ordering inspectable on the CPU.
Framebuffer extent must match the destination frame; regenerate after resizing.
`Screen::draw_list()` refreshes dirty layout after setters without resetting events.

The renderer batches adjacent shapes and adjacent text while preserving painter
order, including text underneath a later popup. It clips shape/glyph geometry,
grows a reusable vertex buffer, and retains glyph caches across frames. It does
not globally sort by draw type or promise one draw call for an entire screen.
Rounded edges use a shader distance calculation. No render graph is required.

All label geometry is prepared and uploaded once per UI submission, then drawn
in the original box/text/image order. `stats().text_vertex_uploads` is zero for
no visible text and one otherwise, not one upload per control. Unchanged layout,
visibility and enabled setters are no-ops; natural label widths cache font shaping
and invalidate on text, font or size changes.

UI draws disable depth test/write and culling and use premultiplied-alpha blending.
The target is not cleared. The UI submission shares one frame command context
across shapes, images and text. Existing parent handles remain valid; when drawing
other geometry afterward, select its program and required state through the same
handle. Frame end closes the recording scope; it does not restore
arbitrary caller-managed GL state. Use an explicit `opengl::RenderStateScope`
when integrating with code that requires that restoration. GPU creation,
use, cache clearing and destruction require the owning context/thread, just like
the other engine renderers. Submission errors are reported but framebuffer writes
already made by earlier batches are not rolled back.

## Text editing and current limits

Single-line fields and multiline areas support committed UTF-8 input, mouse
selection, arrows/Home/End, Backspace/Delete, select-all, clipboard, and bounded
undo/redo (128 edits). CRLF is normalized; tabs become four spaces. ICU supplies
grapheme boundaries, so deletion handles combining sequences and joined emoji
as units even if the selected font cannot display that character.

Multiline areas currently use **explicit newlines, not automatic word wrapping**.
HarfBuzz shapes text; caret positions use glyph advances/clusters with proportional
stops inside ligatures. There is no complete bidirectional-paragraph editing,
font fallback, color emoji, native IME preedit/composition UI, accessibility bridge,
password-field mode, double-click word selection or rich text yet. The input API
distinguishes committed text from physical keys so these can be extended without
turning key codes into text. See [text rendering](text_rendering.md) for shaping
and rasterization limits.

## Tests

`vng_ui_tests` covers polling, focus/capture, subtree lifetime, false/empty changes,
Unicode editing, clipboard/undo, typed dropdowns, clipping, scrolling and DPI.
`vng_window_glfw_tests` covers independent input queues and moves without a GL
context. `vng_ui_opengl_tests` exercises actual shader compilation, rounded shapes,
opacity, text/shape painter order, scaled clips, warm caches, input preservation,
context compatibility and driver diagnostics. GL tests skip through CTest if no
compatible context is available.
