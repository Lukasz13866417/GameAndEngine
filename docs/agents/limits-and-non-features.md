# Limits and non-features

Read this when a scene, key bake, mesh or render fails with a "limit" message, before you plan work that
grows a scene, or before you go looking for a feature (transparency, textures, culling, compute...).

**TL;DR**
- The limits that bite first are **64 MiB per `.vscene`**, which applies both on disk and decoded, and **16,384 timeline
  keys** in total, with **4,096 per track**. Meshes cap at **196,608 vertices**.
- Measure a scene with `./build/vng_scene_probe SCENE --size` (~20 s). Current usage is in [status.md](status.md).
- Value ranges are checked when a file is loaded, not just when it is edited. One bad value fails the whole scene. Editor
  preferences are the exception: they only block growth.
- The renderer draws opaque meshes only: no textures, no culling, no MSAA and no camera roll. See "Does not exist" below.
- Limits are C++ constants. The Source column names the file and symbol; grep the symbol, because line numbers drift.

## Measure before you grow a scene

```sh
cmake --build build --target vng_scene_probe -j 12    # EXCLUDE_FROM_ALL: plain `cmake --build build` skips it
./build/vng_scene_probe examples/assets/tunnel_departure.vscene --size   # ~20 s: it bisects the parser limit
```

It prints three lines: `file N MB, decoded N MiB of the editor's 64 MiB`, then instances, blueprints and tracks, then
`N keys of 16384; longest track N of 4096`. To count the vertices of one mesh, run `grep -m1 '^vertices' FILE.vmesh`.

## Scene documents (`.vscene` editor projects)

| Limit | Value | Source | When exceeded |
|---|---|---|---|
| File size on disk | 64 MiB − 256 B | `include/vng/editor/limits.hpp` `max_document_bytes` (aliased as `max_scene_bytes` in `examples/editor/scene_file.cpp` and `project.cpp`) | Load fails: `Scene exceeds the 64 MiB editor limit` |
| Decoded size | 64 MiB | `examples/editor/project.cpp` `decode()` (`.max_decoded_bytes`) | Load fails in the editor, demos and tools: `Document exceeds the decoded-byte limit`, or `String exceeds the remaining decoded-byte budget` when a string (usually an embedded mesh) crosses it (`src/content/document_parser.cpp`). Check both sizes: decoded can be larger or smaller than the file, depending on content. |
| Encoded output | 64 MiB − 256 B | `project.cpp` `encode_state` | Save, Save As and every `vng_make_*` scene tool fail: `Editor scene exceeds the 64 MiB preview limit` |
| One embedded string (one mesh) | 32 MiB | `decode()` `.max_string_bytes = editor::mesh_limits().max_source_bytes` | `String exceeds the decoded-string byte limit` |
| Preview IPC message | 1 B .. 64 MiB | `max_message_bytes` (limits.hpp); `send_message` in `src/editor/preview.cpp` | `Preview message must contain 1..67108864 bytes`. A whole-scene snapshot is one message, so raising the scene limit means changing the transport. |
| Instances | 65,536 | `examples/editor/authoring_limits.hpp` `max_scene_instances` | Load: `Too many scene instances` (`decode()`). Adding one: `Scene instance limit reached` (`instantiate`). |
| Imported blueprints | 254 mesh + effect together | `project.cpp` `decode()`, `validate_state`, `import_mesh` | Load: `Too many imported mesh blueprints` / `... effect blueprints` (254 of one kind), or `Imported blueprint count/identity exceeds editor limits` (254 combined). Import: `Mesh blueprint limit reached` |
| Instance, blueprint and keyframe names | 256 bytes | `validate_state`; `animation.cpp` `valid_keyframe_name` | Load/edit rejected |
| Project version | writer emits 4; reader accepts 1..4 | `project.cpp` `encode_state`, `decode()` | `Unsupported editor project version` |

- Meshes are embedded as escaped strings. An Earth with a draft counts twice, once for the published mesh and once for the draft.
- A generic `content::Document` (not an editor project) uses the defaults in `include/vng/content/document.hpp` `DocumentLimits`:
  64 MiB source, 128 MiB decoded, 1 Mi nodes, depth 128, 8 MiB strings and 256-byte numbers. See [documents.md](../documents.md).
- `spaceflight.vscene`, `solar_flyby.vscene` and `custom_scene.vscene` are not editor projects. The editor and both
  review tools reject them with `Required property is missing`.

## Timeline

| Limit | Value | Source | When exceeded |
|---|---|---|---|
| Keys per track | 4,096 | `include/vng/timeline/timeline.hpp` `max_keys_per_track` | `Timeline keyframe limit reached` (insert) or `... exceeded` (replace) |
| Keys in total | 16,384 | `max_total_keys` | Same. `KeyBatch::commit` names the largest tracks: `N keys exceed the timeline limit of 16384; largest: ...` |
| Tracks | 16,384 | `max_tracks` | `Timeline track or total keyframe limit reached` |
| Key time | [0, 86,400] s | `max_time` | `Keyframe time must be finite and between 0 and 86400 seconds` |
| Property / label / layer / string value | 256 / 256 / 128 / 4,096 bytes | `max_property_bytes`, `max_label_bytes`, `max_layer_bytes`, `max_string_value_bytes` | Rejected |
| Scene duration | [0.1, 86,400] s | `examples/editor/animation.cpp` `validate_animation` | `Timeline duration must be in [0.1, 86400] seconds` |
| Key times | within [0, duration] | `validate_animation`, `edit_property_keys` | `Animation key time must lie within the timeline duration`. Set `timeline_duration` **before** keying. |
| Named keyframes | 16,384; each name ≤ 256 bytes of UTF-8 with no control characters | `validate_animation` | `Too many named keyframes` / `Keyframe name must be valid UTF-8, ...` |
| Rotation span (KeyBatch only) | after unwrapping, each Euler component must fit in [−360°, 360°] once shifted by whole turns, so its span is at most 720° | `examples/scenes/key_batch.cpp` `KeyBatch::commit` | `Rotation of object N component C turns more than twice around; ...` (span over 720°) or `Rotation of object N axis C spans LOW..HIGH; ...` (no whole-turn shift fits) |

- **Linear interpolation exists only for f32 and Vec3.** Linear bool, int or string keys fail with
  `Linear interpolation requires a float or Vec3 track; discrete values use hold`. `key_property` and `KeyBatch` force bools to `hold`.
- **A track addressing an unknown `(object, property)` pair fails the whole file:**
  `Unknown animated scene property 'P' on object N`. The keyable properties are listed in `animation_properties` (`animation.cpp`).
- A committed rotation track is shifted by whole turns into ±360°. A long free spin therefore needs a pre-rotated mesh or several objects,
  not one long track.

## Instance values (checked on load, on edit and on every key)

| Property | Range | Source |
|---|---|---|
| `position` (each axis), camera target, world bounds | ±1,000,000 | `examples/editor/scene_coordinates.hpp` `scene_coordinate_limit`; `world_bounds.hpp` `valid_world_bounds` (extent ≥ 0.01) |
| `rotation` (each Euler component, degrees) | [−360, 360] | `project.cpp` `valid_transform`; `animation_properties` |
| `scale`, `axis_scale` (each axis) | [0.05, 1,000,000] | `examples/editor/scale_limits.hpp` `min_instance_scale` ... `max_axis_scale` |
| Mesh `brightness` | [0, 5] | `project.cpp` `valid_settings(MeshSettings)` |
| Sun `radius` / `displacement` / `bloom` | [0.1, 4] / [0, 1] / [0, 1] | `examples/editor/effect_presets.cpp` `valid_effect_settings` |
| Sun radius × instance scale | (0, 10,000] | `examples/support/sun_renderer.cpp` `validate_draw` |
| Camera `zoom` | [0.05, 1000] | `examples/editor/camera_limits.hpp` `camera_min_zoom`, `camera_max_zoom` |
| Camera `focus` | [0.001, 1,000,000] | `camera_min_distance`, `camera_max_distance` |
| Active cameras | at most 1 at every keyed time | `camera_instances.cpp` `validate_active_cameras` |

When exceeded:
- In a file, load fails with `Scene instance transform exceeds editor limits` or `Scene instance settings exceed editor limits`.
- On a key, it fails with `Animation key exceeds editor limits for 'P'`.
- A sun over 10,000 fails the **whole frame**: `Sun position must be finite and radius must be in (0,10000]`.
- More than one active camera fails with `Only one camera can be active at a time`.
- Work-arounds used in the voyage (`departure_voyage.cpp`): pre-shrink tiny meshes in the `.vmesh` so that scale can stay ≥ 0.05.
  The sun sits 900,000 km out, inside the ±1,000,000 limit. Its radius is 1.6 at scale 6,000, an effective 9,600, under the 10,000 cap.

## Camera and lens

| Rule | Value | Source |
|---|---|---|
| Vertical FOV | 43° at zoom 1; `2·atan(tan(21.5°)/zoom)` | `examples/editor/project.hpp` `camera_vertical_fov`; `project.cpp` `camera()` |
| Pitch | clamped to ±89.5°, so the camera can't look straight up or down | `project.hpp` `camera_max_pitch` |
| Roll | stored, **ignored** by rendering | `project.hpp` (comment on `CameraSettings`), `camera_pose` in `camera_instances.cpp` |
| Near plane | `min(0.05, distance × 0.25)`, so 50 m in km scenes | `project.cpp` `camera()` |
| Far plane (demos, `vng_scene_frames`) | `environment.view_distance` if > 0, else `max(200, 4 × focus)` | `camera()`, `render_camera` in `animation.cpp` |
| Far plane (editor) | the **Maximum viewing distance** preference: default 10,000, range 1..10,000,000. It ignores `view_distance`. | `examples/editor/settings.hpp`, `settings.cpp` `validate_settings` |
| `vng_scene_frames --look` | near fixed at 0.05. FAR must exceed it or the frame fails after the load. | `examples/tools/scene_frames.cpp` |

## Environment and rendering

| Limit | Value | Source | When exceeded |
|---|---|---|---|
| `stars` | ≤ 20,000 | `validate_state`; `examples/editor/starfield.hpp` | `Scene environment exceeds editor limits` |
| `exposure` / `bloom_threshold` / `bloom_strength` | [0.01, 10] / [0, 100] / [0, 1] | `validate_state` | same |
| `view_distance` | [0, 4,000,000] (4 × the coordinate limit) | `validate_state` | same |
| Readback image (`vng_scene_frames` W H, preview capture) | each side ≤ 8,192 and ≤ 16 Mi pixels (4096×4096 fits; 8192×4096 does not) | `examples/editor/runtime.cpp` `Runtime::prepare_readback` | `Invalid preview readback extent` |

- Lighting per mesh blueprint comes from the `.vmesh` metadata `render/lighting` (`illustrated` plus `render/emission = night`
  selects the night variant). Values are matched exactly and unknown values **silently** fall back to `standard`
  (`examples/editor/mesh_shading.hpp` `lighting_style`).
- Each instance is lit by the single nearest visible sun, treated as a point light. With no visible sun it gets a fixed
  directional light (`Runtime::draw` in `runtime.cpp`).

## Meshes and Earth

| Limit | Value | Source | When exceeded |
|---|---|---|---|
| Vertices per editor mesh | 196,608 | `include/vng/editor/mesh.hpp` `max_mesh_vertices` | `Vertex count exceeds the configured limit` on import, load or embed |
| Faces / edges / scalar values | 262,144 / 524,288 / 4 Mi | `src/editor/mesh.cpp` `mesh_limits` | `Face count ...` / `Edge count ...` / `Vertex scalar count exceeds ...` |
| `.vmesh` source text / decoded | 32 MiB / 32 MiB | `mesh_limits` | `.vmesh source exceeds the configured byte limit` / `... exceeds the configured decoded-byte budget` |
| Earth vertices (terrain + clouds + infrastructure) | 180,224 | `examples/support/earth_infrastructure.hpp` `max_earth_vertices` | `Earth mesh vertex budget exceeded (N / 180224); ...` |
| Earth infrastructure parts | 256 | `examples/support/earth_infrastructure_edit.cpp` | `At most 256 infrastructure parts are supported` |
| Part fields | name ≤ 128 chars; `curve_segments` 8–256; ≤ 14 Bézier controls; ≤ 64 scaffold positions; terminal incline 0–60° | `earth_infrastructure_edit.cpp` `valid()` | Part rejected |
| Skyway endpoints | 0.1°–150° apart | `examples/support/earth_skyway.hpp` | `Tunnel endpoints must be 0.1–150 degrees apart` |

- **Coupled constant:** the departure copies the Bézier controls plus both endpoints into `std::array<..., 16>`
  (`Route::sample_world`, `examples/scenes/tunnel_departure.cpp`). Raising the 14-control limit overflows that array.
- The flat `Builder` in `examples/support/space_assets.cpp` emits 3 vertices per triangle: `quad` 6, `box` and `beam`
  36, `prism` 12 per side, `dome` 4 × rings × segments (560 at the default 7 × 20). Vertex counts grow fast.
- An embedded mesh costs roughly 150–250 B of the 64 MiB decoded budget per vertex (the departure on 2026-09-28:
  267,026 embedded vertices in 56.1 MiB decoded). Sum a scene's embedded vertices with
  `grep -o 'vertices [0-9]* {' SCENE | awk '{s+=$2} END{print s}'`, and confirm any growth with `--size`.

## Editor preferences (authoring budgets, not file limits)

| Preference | Default | Range | Source |
|---|---|---|---|
| Timeline track limit | 256 | 1..16,384 | `authoring_limits.hpp` `default_timeline_track_limit` |
| Instance limit | 4,096 | 1..65,536 | `default_instance_limit` |
| Maximum viewing distance | 10,000 | 1..10,000,000 | `settings.hpp` |
| Undo history | 64 entries | fixed | `EditingSession::remember` (`editing_session.cpp`) |
| Settings file | ≤ 256 bytes | fixed | `settings.cpp` `decode_settings`: `Unknown or oversized editor settings document` |

- Budgets apply only to edits that **grow** the count (`EditingSession::check_track_growth`, `replace`).
  Loading a scene that is already over them works. Growing it fails with
  `Timeline needs N tracks; limit is L. Increase Settings > Timeline track limit.` (and the same pattern for instances).

## Precision (soft limits: nothing fails, the result just degrades)

Everything is `f32`: positions, times and key values. The step between adjacent representable values:

| Magnitude | 1 km | 128 km | 600 km | 6,530 km | 110,000 km | 1,000,000 km | 116 s | 86,400 s |
|---|---|---|---|---|---|---|---|---|
| f32 step | 0.012 cm | 1.5 cm | 6.1 cm | 49 cm | 7.8 m | 62.5 m | 7.6 µs | 7.8 ms |

- Far from the origin, km-scale positions jitter by metres. Stage each location near the origin and move bodies at camera cuts.
  `examples/scenes/departure_voyage.cpp` does this.
- Keyframes are identified by **exact f32 time equality** (`examples/editor/keyframes.cpp`).
- Rotation is per-component linear Euler interpolation (`T·Rz·Ry·Rx·S`), not a slerp. Headings along the scene X
  axis hit gimbal lock. See `orientation()` in `examples/scenes/key_batch.cpp`.

## Outputs never overwrite (except two mesh tools)

| Writer | Behaviour | Source |
|---|---|---|
| `vng_scene_frames` PNGs, demo `--screenshot` | fail if the file exists (`fopen "wbx"`) | `examples/support/presentation.cpp` `write_rgba8_png` |
| Demo `--analyze DIR` | DIR must not exist | `examples/scenes/fleet_inspection.cpp` |
| `SceneFile::save_as`, `vng_make_*` scene tools | refuse unless `--replace`: `Scene destination already exists; confirm replacement before Save As`. The write is atomic and new files get mode 0600. | `examples/editor/scene_file.cpp` |
| Editor **Export** (`.vmesh` or `.veffect`) | `O_EXCL`, never overwrites | `examples/editor/project.cpp` `save_new` |
| `vng_make_earth` | OUTPUT dir must exist and both outputs must be new. `--replace` works only with `--tunnel-classes`, `--redesign-infrastructure` and `--global-infrastructure`. | `examples/tools/make_earth.cpp` |
| `vng_make_spaceship`, `vng_make_fleet` | **truncate silently**, not atomic | `src/content/vmesh.cpp` `write_vmesh` |
| Editor Save / Ctrl+S | overwrites the opened scene in place. Open a copy. | `vng_editor_demo --help` ("Save / Ctrl+S updates the current scene") |

## Time limits

| What | Limit | Source |
|---|---|---|
| e2e tests (`-L e2e`) | CTest TIMEOUT 300 s each, RUN_SERIAL | `CMakeLists.txt` (`vng_editor_e2e_tests` registration) |
| `vng_editor_worker_tests` | TIMEOUT 360 s | `CMakeLists.txt` |
| `vng_editor_runtime_tests` | no TIMEOUT (takes ~90 s in Debug) | `CMakeLists.txt` |
| `vng_editor_demo --once` (with or without `--screenshot`) | gives up after 120 s: `Preview startup timed out: ...` | `examples/editor/app.cpp` |
| Agent tool calls | often 2 min; which commands need a longer timeout or a background run: [build-and-test.md](build-and-test.md#long-commands-and-tool-timeouts) | agent harness |

## Thresholds the tests enforce

| Test | Threshold | Source |
|---|---|---|
| `vng_tunnel_scene_tests` shake | frame-to-frame second difference of the courier's screen position < 3.5 px at 60 fps, over **hard-coded** windows between cuts | `tests/examples/tunnel_scene_tests.cpp` |
| `vng_spaceship_model_tests` | 3,000–12,000 faces | `tests/examples/spaceship_model_tests.cpp` |
| `vng_spaceship_model_tests` (fleet case) | each fleet model 2,500–6,000 faces (exclusive) | `tests/examples/fleet_model_tests.cpp` (compiled into `vng_spaceship_model_tests`) |
| `vng_earth_assets_tests` (plain Earth) | < 46,000 vertices and < 75,000 faces | `tests/examples/earth_assets_tests.cpp` |

When you move a cut, move the test windows with it. Otherwise the tests fail, or pass without checking the new shot.

## Does not exist (don't hunt for it)

**Rendering: editor runtime, demos and review tools**
- **Mesh transparency.** The mesh pipeline sets `BlendMode::disabled` (`BlueprintMeshRenderer::render`,
  `examples/editor/blueprint_mesh_renderer.cpp`). In a scene only the stars, the tunnel portal haze and the sun effect
  (additive, `examples/support/sun_renderer.cpp`) blend; editor overlays and UI text blend too.
- **Textures and per-instance materials in scenes.** `cpu_mesh` (`mesh_shading.hpp`) streams only `position`, `color/0`,
  `normal` and `emission`. The look is chosen per blueprint via `render/lighting`. The *engine* can sample textures
  (`sample_2d`, the textured `MeshRenderer` in `vng_resources_opengl`), but the scene path doesn't use them.
- **Frustum culling, LOD and back-face culling.** Every visible instance is drawn every frame with `CullMode::none`.
  Hide geometry with a `visible` key.
- **MSAA.** Contexts pass `samples = 0` (the default in `include/vng/opengl/context.hpp`). Readback refuses multisample
  attachments, and window presentation needs a linear default framebuffer. MSAA is unsupported and untested.
- **Motion blur, depth of field, shadow maps and camera roll.**
- **Animatable or UI-editable environment.** Stars, exposure, bloom and view distance have no timeline property and no
  editor panel. Edit them in the generator or the file. `render/lighting` also has no editor UI.
- **Star parallax.** Stars are fixed world directions from `star_seed`.

**Engine**
- **Compute, geometry and tessellation stages.** `shader::StageKind` is `vertex` or `fragment` only.
- **DSL control flow.** `if_region`, `loop_region` and `yield` exist in the IR, but the DSL never emits them and GLSL
  lowering rejects them (`operation is not a GLSL value expression`).
- **A math library.** `include/vng/core/types.hpp` defines the vector and matrix types. The operators live in their consumers.
- **Backends other than OpenGL 4.6, and windows other than GLFW.** GPU tests exit 77 (Skipped) without a 4.6 context.
- **Audio, physics and runtime scripting.** Scenes are data. Generators are offline C++, and no callbacks survive into a `.vscene`.
- **A unit system.** "km" is a convention of the space scenes. `vng_scene_probe` prints "km away" whatever the units.
- **A "full scene renderer", materials or a pass graph.** The README says they "deliberately come after this foundation".

**Editor and tools**
- **Non-Linux editor, preview, editor runtime, scene demos or review tools.** `vng_editor_preview` is created only under
  `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` in `CMakeLists.txt`, and those targets sit under `if(TARGET vng_editor_preview)`.
  The engine-only demos (`vng_triangle_demo`, `vng_sun_demo`, `vng_spaceflight_demo`...) are not gated.
- **Headless rendering.** `vng_scene_frames` hides its window but still needs an X display and GL 4.6, and exits 77 otherwise.
- **`--help` on `vng_scene_frames` and `vng_scene_probe`.** Bad arguments print the usage and exit 1. Times are
  parsed with `strtof` and never range-checked or clamped, so `abc` becomes 0 (in `vng_scene_frames` only a non-finite time fails, at render).
- **Independent Play without a camera instance.** The editor says `Add a camera first (Blueprints > Camera +)`.
- **Automatic regeneration of committed scenes.** `cmake --build` never rewrites `examples/assets/*.vscene`. The scene
  tests author their scenes in memory from the generators; a few tests read committed files (`editor_timeline.vscene`,
  `fleet_reveal.vscene`, `earth_future.*`), but none reads `tunnel_departure.vscene`, `asteroid_fleet.vscene` or
  `solar_system.vscene`. A stale copy of those still passes every test.

**Project and process**
- **CI, git hooks, `-Werror`, a formatter or a linter config** (`.clang-format`, `.clang-tidy`, `.editorconfig`).
  The only gates are the tests you run and the owner's PR review. `-Wall -Wextra -Wpedantic -Wconversion` produce warnings only.
- **Git LFS.** Multi-MB assets are plain git blobs, and every regeneration adds a new one.
- **Checks on topic docs.** Only `docs/explore/*` excerpts and links, the links in `docs/codebase.md`, and the editor
  role class names in `docs/editor_components.md` are tested (`vng_guide_tests`). The rest of `docs/*.md` and the
  READMEs can drift silently.

## See also
- [status.md](status.md): current scene usage against these limits, and known failures.
- [../tunnel.md](../tunnel.md), [../timeline.md](../timeline.md), [../documents.md](../documents.md),
  [../earth.md](../earth.md), and the "Editor settings" section of [../editor.md](../editor.md).
