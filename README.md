# Vibe Engine

New to the project? Open [the interactive architecture guide](docs/explore/index.html)
in a browser. Expand one branch at a time for plain-language explanations,
small API examples, and links to the actual source. It works offline without a server.

`vng` is a C++23 game-engine experiment built to keep the renderer's data and
code-generation paths visible.  The current vertical slice provides:

- semantic, explicitly encoded vertex records and owning vertex streams;
- typed single- or multi-stream meshes with triangle faces and optional edges;
- a backend-neutral perspective/orthographic camera with typed shader inputs;
- backend-selected compiled programs and typed, per-draw graphics state setters;
- an empty, non-virtual `render::Renderer<Ticket, Backend>` contract, with the
  explicit `opengl::Renderer<Ticket>` shorthand for OpenGL policies,
  immutable render views, and target-scoped backend frames;
- a strict, human-readable `.vmesh` format and typed semantic-schema bridge;
- generic structured documents with typed property access, custom decoders,
  scoped readers, and canonical unknown-property-preserving writing;
- typed OpenGL mesh uploads that own their buffers and infer/cache VAOs from
  the linked shader program;
- compile-time multi-stream and instanced vertex-layout validation;
- an immutable-expression shader DSL that builds backend-neutral typed IR;
- typed shader-lambda arguments supplied as ordinary CPU values at draw time,
  with implicit expression operands restricted to compile-time constants;
- deterministic GLSL 4.60 generation;
- font shaping and a ticket-based text renderer with cached glyph atlases,
  per-ticket fonts/sizes/colors, clipping, and ordered batching;
- retained UI widgets with polling, themes, Unicode text editing, typed dropdowns,
  logical-pixel layout and a backend-selected renderer;
- a two-process mesh/effect/scene editor with C++-described controls, vertex editing,
  undo, shared-memory preview and rebuild/restart without losing authored state;
- independent armatures, mesh/weight bindings, and poses with a renderer-owned
  four/eight-influence GPU skinning path;
- reusable resource providers, ready-resource transfer and transactional
  mesh/program/image, font and skin replacement;
- PNG/P6 texture loading, coherent HDR render targets, and a DSL bloom effect
  with resize and reload support;
- opt-in enhanced shader emission and an analysis render path that captures
  color, device depth, and per-pixel source-surface identity;
- backend-neutral frame-evidence summaries, pixel inspection, comparison,
  diagnostic visualizations, and deterministic JSON/image export;
- mutually independent GLFW window-system and OpenGL 4.6 backends, joined only
  by the optional GLFW + OpenGL integration target.

Materials, a full scene renderer, and a pass graph deliberately come after this
foundation.

Run `./build/vng_editor_demo` after building that target for the editable sun/mesh
project. See [editor architecture and controls](docs/editor.md). Set
`VNG_BUILD_EDITOR=OFF` to omit the editor modules and applications.

The editor includes [property timelines](docs/timeline.md) with per-key incoming
interpolation, scrubbing, layer filtering, undo and scene persistence. Try
`./build/vng_editor_demo --scene examples/assets/editor_timeline.vscene`.

For an editable cinematic scene, run `./build/vng_fleet_reveal_demo`: a Kestrel
rounds the sun to reveal a 17-ship formation. Open the same
`examples/assets/fleet_reveal.vscene` in the editor to tune ships and camera keys.
See [fleet reveal](docs/fleet_reveal.md) for build/run commands and diagnostics.

`vng_asteroid_fleet_demo` offers a second editable shot: fly through a dense,
cratered asteroid belt to reveal the fleet. Open `examples/assets/asteroid_fleet.vscene`
in the editor. See [asteroid fleet](docs/asteroid_fleet.md).

`vng_solar_system_demo` combines Earth, the sun, the belt and the fleet into one
establishing shot with only its starting keyframe: the camera hangs beside Earth
looking out at the distant belt, with the fleet waiting in front of it. Open
`examples/assets/solar_system.vscene` in the editor to author the departure.
See [leaving home](docs/solar_system.md).

See [typed shader arguments](docs/typed_shader_arguments.md) for runtime values,
constant-only expression syntax, semantic parameter records, and backend lowering.

The CPU-only `vng_document_demo` reads `examples/assets/custom_scene.vscene`
without imposing scene/object schemas. It demonstrates `get<float>("emission")`,
custom typed decoding, and source-aware diagnostics. See
[structured documents](docs/documents.md) for the low-level and scoped-reader APIs.

```sh
cmake --build build --target vng_document_demo
./build/vng_document_demo
```

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

GLAD, Catch2, FreeType, and HarfBuzz prefer installed packages and otherwise are
fetched at pinned revisions. GLFW does too except on Linux, where the pinned
source includes our [X11 fractional-scroll fix](cmake/glfw/README.md).
UI also requires installed ICU 67+
development files (`libicu-dev`); set `VNG_BUILD_UI=OFF` to omit UI/ICU.
Set `VNG_BUILD_TEXT=OFF` to omit font dependencies, text rendering and UI.
Image decoding also requires installed
libpng development files. OpenGL tests create a hidden GLFW
window; CTest reports them as skipped when a 4.6 context cannot be created.

Build only the backend-neutral layers, including CPU font shaping, with:

```sh
cmake -S . -B build-core \
  -DVNG_BUILD_OPENGL=OFF \
  -DVNG_BUILD_WINDOW_GLFW=OFF
```

For a headless or non-GLFW context provider, keep `VNG_BUILD_OPENGL=ON` and set
`VNG_BUILD_WINDOW_GLFW=OFF`; the OpenGL target does not discover or link GLFW.

The primary architecture example is `vng_file_mesh_demo`. Its short
`file_mesh.cpp` walkthrough loads `examples/assets/colored_cube.vmesh` and
hands it to the adjacent `FileMeshRenderer : opengl::Renderer<FileMeshDraw>`. The
renderer's factory creates its shaders, compiles its shader runtime, uploads
the CPU/GPU mesh pair; application code does not supply a shader or persistent
state preset. Its `render` method uses the OpenGL frame's command stream to
select a program, upload the view, change depth/cull state per ticket,
and draw. An alternate `.vmesh` path can be passed as its first argument. Use
`--once` for a one-frame smoke run or `--frames N` for a bounded run. Repeated
command-line, diagnostics, startup, reporting, and window-loop code lives
under `examples/support`; it is example-only glue, not an engine API.

`vng_triangle_demo` deliberately uses `make_simple_mesh_renderer`, the optional
one-mesh/one-program convenience for tiny examples. It is not the renderer
architecture. `vng_file_mesh_direct_demo` renders the same cube without a
renderer and shows both shader stages, program compilation, GPU upload, and
typed `graphics.set(...)` calls directly. `vng_advanced_instanced_streams_demo` goes one level
lower by preserving the explicit stream-resolution/VAO path.

```sh
cmake --build build --target vng_file_mesh_demo
./build/vng_file_mesh_demo --once
./build/vng_file_mesh_demo --analyze --once
./build/vng_file_mesh_demo path/to/another.vmesh
./build/vng_file_mesh_direct_demo --once
```

`vng_text_demo` loads the bundled DejaVu Sans font and renders text tickets;
the renderer owns its shaders, atlas, and batching. See
[text rendering](docs/text_rendering.md) for the API and its current limits.

```sh
cmake --build build --target vng_text_demo
./build/vng_text_demo
./build/vng_text_demo --once
```

`vng_rigging_demo` binds a procedural mesh to three bones and renders it with
two independently animated poses. Its renderer owns the DSL shaders and GPU
resources; only matrix palettes change during animation. See
[character rigging](docs/rigging.md) for the public APIs, ownership rules, and
dependency tree.

```sh
cmake --build build --target vng_rigging_demo
./build/vng_rigging_demo
./build/vng_rigging_demo --once
```

`vng_ui_demo` exercises buttons, checkboxes, dropdowns, text fields and scrolling.
Create widgets once, poll `clicked()` / `changedText()` after each update, then
submit the screen to its renderer. See [UI](docs/ui.md) for themes, input routing,
backend boundaries and current text-editing limits.

```sh
cmake --build build --target vng_ui_demo
./build/vng_ui_demo
```

`vng_glow_demo` uses reusable mesh/image providers, emissive draws, a retained
HDR render target and bloom. `B` toggles bloom, `Space` pauses animation, `R`
reloads resources, and `Esc` closes the window. Text is an optional HUD; the
scene builds with `VNG_BUILD_TEXT=OFF` too. See
[resource ownership](docs/resources.md) and [bloom](docs/bloom.md) for the APIs,
frame sequence and current rendering limits.

```sh
cmake --build build --target vng_glow_demo
./build/vng_glow_demo
./build/vng_glow_demo --once
./build/vng_glow_demo --frames 120 --reload
./build/vng_glow_demo --no-bloom
```

`vng_spaceflight_demo` loads an original detailed spaceship and a `.vscene`
document for a lit, blooming flyby. The shot starts with empty space; wait
about five seconds for the ship to enter. It needs no text support. See
[spaceflight](docs/spaceflight.md) for playback controls, reproducible screenshots
and enhanced-render diagnostics.

```sh
cmake --build build --target vng_spaceflight_demo
./build/vng_spaceflight_demo
```

`vng_sun_demo` renders an animated, procedurally generated sun: true radial
surface displacement, fine granulation, active regions, prominence strands,
and an HDR corona with bloom. It demonstrates typed shader arguments and
vertex-stage texture sampling without rebuilding shaders or geometry each
frame. `Space` pauses, `B` toggles bloom, `D` toggles displacement, `R` resets,
and `Esc` closes. This is a cinematic false-color visualization, not a physical
solar simulation. See [sun](docs/sun.md) for reproducible screenshots and
enhanced surface diagnostics.

```sh
cmake --build build --target vng_sun_demo
./build/vng_sun_demo
./build/vng_sun_demo --time 3 --screenshot /tmp/sun.png --analyze /tmp/sun-evidence
```

## Architecture

The shader build lambda executes once and records typed operations. It does not
emit GLSL and is not replayed per backend:

```text
C++ DSL -> typed structured IR -> validation -> GLSL 4.60 -> OpenGL compiler
```

The same validated program can also be lowered through opt-in analysis or
selective-observation emission. `OpenGLProgramRuntime::capture` generates and caches
those products lazily, draws to private attachments, and returns a portable
evidence bundle. The ordinary shader IR and GLSL product remain unchanged and
pay no analysis cost.

Mesh content and vertex storage follow an independent path until a concrete
renderer uploads the mesh and the linked program supplies the shader-input
contract:

```text
.vmesh -> Document + Schema -> gfx::Mesh<Record...>
                            -> concrete renderer creation
                            -> backend-owned GPU mesh + cached VAO
Frame -> Render context -> run / view / graphics.set / draw
```

The ordinary mesh API never exposes stream resolution, raw vertex buffers, or
VAO construction. Those remain backend implementation details.

Window event pumping is exposed through the window object, so a frame loop
uses `window.poll_events()` and does not name GLFW after backend selection.

See [docs/shader_vertex_pipeline.md](docs/shader_vertex_pipeline.md) for the
shader/vertex API and internal transitions. See
[docs/mesh_and_vmesh.md](docs/mesh_and_vmesh.md) for runtime mesh ownership,
the text grammar, and typed file decoding. See [docs/camera.md](docs/camera.md)
for camera conventions, validation, and renderer use. See
[docs/graphics_state.md](docs/graphics_state.md) for program selection, per-draw
settings, backend extensions, and state-handle lifetimes. The
[program API](docs/graphics_program.md) owns shaders independently of those settings. See
[docs/renderer_api.md](docs/renderer_api.md) for tickets, custom renderer
policies, frame commands, views, and persistent renderer ownership. See
[docs/window_and_context.md](docs/window_and_context.md) for the neutral window
description, OpenGL context description, integration factory, and threading
rules. See
[docs/analysis_rendering.md](docs/analysis_rendering.md) for enhanced shader
emission, capture lookup, and the current analysis-rendering boundary. See
[docs/analysis_evidence.md](docs/analysis_evidence.md) for portable metadata,
automatic summaries, rich inspection, comparison, and directory export.
