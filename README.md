# Vibe Engine

`vng` is a C++23 game-engine experiment built to keep the renderer's data and
code-generation paths visible.  The current vertical slice provides:

- semantic, explicitly encoded vertex records and owning vertex streams;
- typed single- or multi-stream meshes with triangle faces and optional edges;
- a backend-neutral perspective/orthographic camera with typed shader inputs;
- backend-neutral graphics-pipeline descriptions with backend-selected
  compiled pipeline objects;
- an empty, non-virtual `Renderer<Ticket>` base for user-defined policies,
  immutable render views, and target-scoped backend frames;
- a strict, human-readable `.vmesh` format and typed semantic-schema bridge;
- typed OpenGL mesh uploads that own their buffers and infer/cache VAOs from
  the linked shader program;
- compile-time multi-stream and instanced vertex-layout validation;
- an immutable-expression shader DSL that builds backend-neutral typed IR;
- deterministic GLSL 4.60 generation;
- opt-in enhanced shader emission and an analysis render path that captures
  color, device depth, and per-pixel source-surface identity;
- backend-neutral frame-evidence summaries, pixel inspection, comparison,
  diagnostic visualizations, and deterministic JSON/image export;
- mutually independent GLFW window-system and OpenGL 4.6 backends, joined only
  by the optional GLFW + OpenGL integration target.

Materials, a full scene renderer, and a pass graph deliberately come after this
foundation.

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

GLAD, GLFW, and Catch2 are found as installed CMake packages when available and
otherwise fetched at pinned revisions.  OpenGL tests create a hidden GLFW
window; CTest reports them as skipped when a 4.6 context cannot be created.

Build only the backend-neutral core, gfx, render, content, shader, and GLSL
layers with:

```sh
cmake -S . -B build-core \
  -DVNG_BUILD_OPENGL=OFF \
  -DVNG_BUILD_WINDOW_GLFW=OFF
```

For a headless or non-GLFW context provider, keep `VNG_BUILD_OPENGL=ON` and set
`VNG_BUILD_WINDOW_GLFW=OFF`; the OpenGL target does not discover or link GLFW.

The primary architecture example is `vng_file_mesh_demo`. Its short
`file_mesh.cpp` walkthrough loads `examples/assets/colored_cube.vmesh` and
hands it to the adjacent `FileMeshRenderer : Renderer<FileMeshDraw>`. The
renderer's factory creates its shaders, compiles its shader runtime, uploads
the CPU/GPU mesh pair, and establishes its baseline state; application code
does not supply a shader. Its `render` method uses the OpenGL frame's command
stream to bind a program, upload the view, change depth/cull state per ticket,
and draw. An alternate `.vmesh` path can be passed as its first argument. Use
`--once` for a one-frame smoke run or `--frames N` for a bounded run. Repeated
command-line, diagnostics, startup, reporting, and window-loop code lives
under `examples/support`; it is example-only glue, not an engine API.

`vng_triangle_demo` deliberately uses `make_simple_mesh_renderer`, the optional
one-mesh/one-pipeline convenience for tiny examples. It is not the renderer
architecture. `vng_file_mesh_direct_demo` renders the same cube without a
renderer and shows both shader stages, pipeline compilation, GPU upload, and
frame commands directly. `vng_advanced_instanced_streams_demo` goes one level
lower by preserving the explicit stream-resolution/VAO path.

```sh
cmake --build build --target vng_file_mesh_demo
./build/vng_file_mesh_demo --once
./build/vng_file_mesh_demo --analyze --once
./build/vng_file_mesh_demo path/to/another.vmesh
./build/vng_file_mesh_direct_demo --once
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
Frame -> Commands -> bind / view / state / draw
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
[docs/graphics_pipeline.md](docs/graphics_pipeline.md) for neutral pipeline
descriptions and backend realization. See
[docs/renderer_api.md](docs/renderer_api.md) for tickets, custom renderer
policies, frame commands, views, and persistent renderer ownership. See
[docs/window_and_context.md](docs/window_and_context.md) for the neutral window
description, OpenGL context description, integration factory, and threading
rules. See
[docs/analysis_rendering.md](docs/analysis_rendering.md) for enhanced shader
emission, capture lookup, and the current analysis-rendering boundary. See
[docs/analysis_evidence.md](docs/analysis_evidence.md) for portable metadata,
automatic summaries, rich inspection, comparison, and directory export.
