# Examples

The example entry points keep engine operations visible and delegate only
presentation concerns to `support/`.

- `triangle.cpp` is the smallest end-to-end path: map a `.vmesh` schema, build
  a shader with the DSL, create the simple renderer, and submit one draw.
- `file_mesh.cpp` is the application-level walkthrough: load a 3D mesh, hand
  it to a custom renderer, optionally diagnose a frame, and render it. The
  renderer's shaders, ticket, resources, and command policy are isolated in
  `file_mesh_renderer.hpp/.cpp`; its factory owns shader creation.
- `file_mesh_direct.cpp` renders the same cube without a renderer. It keeps the
  two DSL shader stages, program compilation, GPU upload, and typed graphics
  state setters visible for comparison with the renderer-owned path. Its
  `Float4x4 model` and `Float brightness` lambda arguments receive ordinary CPU
  values through `commands.run(program, model, brightness)`.
- `text.cpp` loads the bundled font and submits text tickets with different
  sizes and colors to the engine's text renderer. The renderer owns its
  shaders, glyph atlas, and batching; the example needs no glyph geometry.
- `ui.cpp` creates retained controls, polls their state and renders a screen.
  The renderer owns shape shaders and text batching; no per-widget drawing code
  or callback registration is required.
- `editor.cpp` starts the two-process mesh/effect/scene editor. `editor/` keeps
  authored data, generic inspector widgets and viewport tools separate from the
  worker-only renderer and native callbacks; `editor_worker.cpp` is the reloadable
  preview process. Open `assets/editor_timeline.vscene` with `--scene` for keyed
  mesh motion, visibility switches and animated sun properties. See
  [the editor guide](../docs/editor.md) and [timeline API](../docs/timeline.md).
  The `vng_editor_e2e_tests` target walks through the real UI and worker with
  semantic control locators, pixel/layout checks and a screenshot gallery;
  artifacts are saved to a unique directory under `build/editor-e2e/`.
  **Import...** accepts meshes and saved effect presets; try
  [`assets/quiet_sun.veffect`](assets/quiet_sun.veffect). Select a Sun instance
  and use **Export new .veffect** to reuse its settings in another scene.
- `rigging.cpp` builds an armature, binds a mesh, edits two independent poses,
  and submits skinned draw tickets. Procedural tube construction lives in
  `support/rigging_scene.hpp`, keeping the entry point focused on rigging APIs.
- `glow.cpp` builds textured/emissive meshes from retained providers, renders
  to a coherent HDR target, and composites bloom into the output frame. It
  demonstrates target resize and resource reload with an optional text HUD.
- `spaceflight.cpp` loads an original spaceship and a `.vscene` document, then
  stages a lit, blooming flyby. Asset/scene parsing and renderer-owned shaders
  live in separate helpers; screenshots and enhanced-shader diagnostics are
  available for repeatable inspection.
- `fleet_reveal.cpp` plays the editable `assets/fleet_reveal.vscene`: 17 ships,
  four mesh blueprints, and a moving-camera reveal around the sun. It uses the
  editor's same runtime; camera and ship motion are saved timeline keys, not
  demo-only callbacks. See [fleet reveal](../docs/fleet_reveal.md) for running,
  editing, reproducible captures, and the offline scene generator.
- `asteroid_fleet.cpp` is the second editable fleet shot: a close flight through
  600 separated tumbling rocks spread through a deep belt, partially obscuring
  a distant 40-ship fleet before the reveal;
  the pathfinder brakes to a stop rather than joining it. Three generated rock
  blueprints are embedded in `assets/asteroid_fleet.vscene`. Both fleet demos
  share playback glue and the editor's Runtime. See [asteroid fleet](../docs/asteroid_fleet.md).
- `earth.cpp` plays `assets/earth.vscene`, an editable turntable of an original
  stylized Earth mesh blueprint. Import `assets/earth.vmesh` into any scene;
  smooth normals, raised clouds and opt-in illustrated DSL lighting use the
  same instanced renderer in the editor and demo. See [Earth](../docs/earth.md).
- `sun.cpp` submits time/displacement tickets to a renderer that owns its
  procedural solar geometry, data map, and DSL shaders. The surface really
  displaces radially; animated prominence strands, a corona, subtle softening, and HDR bloom
  complete the shot. Surface and arcs share one slow rotation; denser, overlapping
  strands blend into plasma bundles. Options, asset generation, shaders, and diagnostic export
  live in `support/sun_*` helpers.
- `document.cpp` is a CPU-only walkthrough of generic structured documents:
  typed getters, custom decoders, scoped readers, and different object-specific
  attributes. It preserves unknown properties during canonical round trips;
  it does not create a scene or GPU resources.
- `advanced_instanced_streams.cpp` intentionally exposes the escape hatch:
  resolve a multi-stream layout, upload raw buffers, configure a VAO, and issue
  an instanced OpenGL draw.

Both mesh paths select shaders with `context.run(program, arguments...)` and change depth
testing/writing independently through `graphics.set(render::DepthTest{...})`
and `graphics.set(render::DepthWrite{...})`. No state preset is required.
See [typed shader arguments](../docs/typed_shader_arguments.md) for the
constant-only operand rule, parameter records, and enhanced-render snapshots.

`support/` contains example-only helpers: command-line parsing, diagnostic/report
formatting, GLFW+OpenGL startup, window-loop bookkeeping, and each larger demo's
procedural assets and rendering policy. These helpers are linked only by demos
and their regression tests; they are not an engine API.

Build and run the structured-document example with:

```sh
cmake --build build --target vng_document_demo
./build/vng_document_demo
./build/vng_document_demo examples/assets/custom_scene.vscene
```

It needs no OpenGL, GLFW, or text support. The printed type-error diagnostic
at the end is intentional. See [structured documents](../docs/documents.md).

Build and run the text example with:

```sh
cmake --build build --target vng_text_demo
./build/vng_text_demo
./build/vng_text_demo --once
```

It requires `VNG_BUILD_TEXT`, OpenGL, and GLFW. Font licensing is included in
`assets/fonts/`. See [text rendering](../docs/text_rendering.md) for ticket
coordinates, measurement, batching, and cache limits.

Build and run the UI example with:

```sh
cmake --build build --target vng_ui_demo
./build/vng_ui_demo
./build/vng_ui_demo --screenshot /tmp/ui-new.png
```

It requires UI/text support, ICU, OpenGL and GLFW. Controls are created once,
then polled after `ui::update()`. See [UI](../docs/ui.md) for input routing,
themes, ownership, dependency direction and current text-editing limits.

Build and run the skeletal-deformation example with:

```sh
cmake --build build --target vng_rigging_demo
./build/vng_rigging_demo
./build/vng_rigging_demo --once
```

It requires OpenGL and GLFW, but not text/font dependencies. The two characters
share geometry, armature, and weights while their poses and placements differ.
See [character rigging](../docs/rigging.md) for asset boundaries, four/eight
influences, diagnostics, and the dependency tree.

Build and run the HDR/glow example with:

```sh
cmake --build build --target vng_glow_demo
./build/vng_glow_demo
./build/vng_glow_demo --once
./build/vng_glow_demo --frames 120 --reload
./build/vng_glow_demo --no-bloom
```

Press `B` to toggle bloom, `Space` to pause animation, `R` to reload resources,
or `Esc` to close. `--no-bloom` still runs exposure and tone mapping, making
the effect comparison useful. OpenGL and GLFW are required; the scene works
with `VNG_BUILD_TEXT=OFF`, without the text HUD.

The mesh renderer is unlit and emissive, with no PBR lighting or scene-wide
asset manager. See [resource providers](../docs/resources.md) for sharing,
ready-resource adoption and update transactions, and [bloom](../docs/bloom.md)
for the two-frame HDR/output sequence and effect settings.

Build and run the file-backed spaceship shot with:

```sh
cmake --build build --target vng_spaceflight_demo
./build/vng_spaceflight_demo
./build/vng_spaceflight_demo --time 10.7
```

The opening intentionally shows only stars; the ship enters after about five
seconds and the 16-second shot loops. `Space` pauses, `R` restarts, `B` toggles
bloom, and `Esc` closes. `--once` alone therefore shows the empty opening.
Screenshot/analysis requests default to the close-up instead. This example
requires OpenGL and GLFW, not text support. See [spaceflight](../docs/spaceflight.md)
for safe capture commands, file formats, the model generator and the distinction
between final screenshots and ship-only pre-bloom diagnostics.

Build and run the procedural solar close-up with:

```sh
cmake --build build --target vng_sun_demo
./build/vng_sun_demo
./build/vng_sun_demo --time 3 --screenshot /tmp/sun.png --analyze /tmp/sun-evidence
./build/vng_sun_demo --no-displacement
./build/vng_sun_demo --white-spots
```

The sun is visible immediately. `Space` pauses, `B` toggles bloom, `D` toggles
radial displacement, `W` toggles white-hot surface spots (off by default), `R` restarts animation,
and `Esc` closes. `--time` fixes the
sample, so pause/reset controls cannot change its time. Capture flags default
to one frame at three seconds and require new output paths. This is a cinematic
false-color visualization, not a physical plasma simulation. It needs OpenGL
and GLFW but no text support. See [sun](../docs/sun.md) for implementation
boundaries and the distinction between final screenshots and surface-only
enhanced diagnostics.

For a shared scene with the sun on the left and the ship emerging from beneath
the camera, banking right and receding around the sun:

```sh
cmake --build build --target vng_solar_flyby_demo
./build/vng_solar_flyby_demo
```

The ship first appears around five seconds. `Space` pauses, `R` restarts the
24-second flyby, `B` toggles bloom, and `W` toggles the sun's white spots.
The camera remains fixed and the shot holds at the end rather than jumping back.
The composition and curve are in `assets/solar_flyby.vscene`; no mesh regeneration
is needed to edit them. See [solar flyby](../docs/solar_flyby.md) for captures and
the separate ship/sun enhanced-rendering exports.
