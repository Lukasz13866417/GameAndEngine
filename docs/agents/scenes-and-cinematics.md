# Scenes and cinematics in code

Read this when you add or change a scene generator (`examples/scenes/`), key motion or cameras from C++, change a
shot of the departure cinematic, or regenerate a shipped `.vscene`. For how a scene becomes pixels and the full review
loop, see [rendering-and-review.md](rendering-and-review.md). For meshes and the Earth asset, see [assets.md](assets.md).

**TL;DR**
- A scene is a `.vscene`, which is a saved `editor_example::State`. A generator is offline C++ (`author_scene(assets)`)
  that a `vng_make_*` tool saves. None of it runs at playback. The CPU scene tests call `author_scene` and never read
  the shipped file.
- Bake motion into linear and hold keys with `example::KeyBatch`, one validated commit. A cut is a `hold` key at T,
  preceded by the outgoing shot's key at T − 0.001 s. Key the camera's eye directly with `key_look`.
- Sample fast motion at 1/30 to 1/60 s. Stage close action near the origin (f32: 1.5 cm spacing at 128 km, 6 cm at
  600 km, where the departure stages it). Keep craft headings off the ±X axis (gimbal lock).
- Verify with `vng_scene_probe` (numbers), then `vng_scene_frames` (pixels), then the scene's CPU test (shake < 3.5 px).
- Reference implementation: `examples/scenes/tunnel_departure.cpp` + `departure_voyage.cpp`.

**Precondition.** Run `ls examples/scenes/key_batch.hpp examples/tools/scene_frames.cpp examples/tools/scene_probe.cpp`.
If any is missing, your base predates the cinematic or its review tools; [status.md](status.md) says which branch has
them. Also run
`grep -q 'high > 360.F ?' examples/scenes/key_batch.cpp && echo "has the KeyBatch camera fix" || echo "no KeyBatch camera fix"`.
Without the fix, any batch-keyed camera whose `rotation.x` stays at or below 0 all shot (it looks level or down, e.g.
an orbit above its subject) renders pointing straight up (§6). [status.md](status.md) says which branch has the fix.

## 1. Mental model

```
examples/support/*.cpp    CPU mesh recipes -> content::vmesh::Document     (may not include ../editor or ../scenes)
examples/scenes/*.cpp     author_scene(assets) -> editor_example::State   (instances, one timeline, camera, environment)
examples/tools/make_*_scene.cpp, make_tunnel_departure.cpp
                          SceneFile::save_as -> examples/assets/*.vscene  (refuses to overwrite without --replace)
        |  loaded by: vng_editor_demo, vng_<name>_demo, vng_scene_frames, vng_scene_probe
tests/examples/*_tests.cpp  call author_scene directly
```

- Treat a cinematic as a pure function of time. You evaluate it at sample times you choose and store the results as
  keys. Keys are linear or hold; there are no curves, so shape comes from sampling density.
- The camera is an ordinary instance (`BlueprintId::camera`). Demos and tools render it through
  `render_camera(state, t)`. The editor uses its own far plane (see §5.3).
- Everything is f32: positions, times and Euler angles. The budgets are 16,384 keys, 4,096 keys per track and 64 MiB per
  scene. Full list: [limits-and-non-features.md](limits-and-non-features.md).

## 2. Where things are

| Path | What it holds |
|---|---|
| `examples/scenes/key_batch.hpp`, `.cpp` | The whole bulk-keying API: `KeyBatch`, `Look`, `key_look`, `orbit_pose`, `orientation` |
| `examples/editor/animation.hpp` | `animation_properties` (what is keyable, with ranges), `key_property`, `edit_property_keys`, `ensure_camera`, `key_camera`, `render_camera`, `active_camera`, `evaluate_instance`, `validate_animation` |
| `examples/editor/project.hpp` | `State`, `Document`, `SceneInstance`, `CameraSettings`, `InstanceTransform`, `place_camera`, `validate_active_cameras`, `find_instance`, `erase_instance` |
| `include/vng/timeline/timeline.hpp` | `Value` (bool, i32, u32, f32, Vec3, string), `Interpolation {hold, linear}`, key limits. Sampling is in `Timeline::sample` in `src/timeline/timeline.cpp`. |
| `examples/scenes/tunnel_scene.cpp` | A small complete generator: a camera instance through `ensure_camera`/`key_camera`, validation, and a hand-filled `vmesh::Document` (`document()`) |
| `examples/scenes/tunnel_departure.{hpp,cpp}` | The skyway act (0 to ~19.4 s), then `author_scene` for the whole 116 s: blueprints, camera instance, `simplify`, `commit`, environment, bounds |
| `examples/scenes/departure_voyage.{hpp,cpp}` | The voyage after the handoff: Earth orbit, Moon and belt. Authoring helpers are in §5.7. |
| `examples/scenes/{fleet,asteroid,solar_system,earth}_scene.cpp` | Older, smaller generators that use `key_property` |
| `examples/tools/make_*_scene.cpp`, `make_tunnel_departure.cpp` | Scene CLI wrappers: `ASSET_DIRECTORY SCENE.vscene [--replace]`. `make_earth`, `make_fleet` and `make_spaceship` write meshes instead: see [assets.md](assets.md). |
| `examples/tools/scene_probe.cpp`, `scene_frames.cpp` | Offline review tools |
| `tests/examples/tunnel_scene_tests.cpp` | How cinematics are asserted numerically (§7) |
| `CMakeLists.txt`, block `if(TARGET vng_editor_project)` under `if(VNG_BUILD_EXAMPLES)` | Scene libraries, the scene and Earth `vng_make_*` tools, CPU scene tests. Demos and review tools are in the later `if(TARGET vng_editor_preview)` block. |
| [docs/tunnel.md](../tunnel.md), [docs/solar_system.md](../solar_system.md) | Human docs: the departure's beat table, and a template for a new shot's doc |

## 3. Commands

Run from the repo root, Debug build in `./build`. For configuring, see [build-and-test.md](build-and-test.md).
Every `vng_make_*` tool and both review tools are `EXCLUDE_FROM_ALL`, so name them when you build. Times below are
for the 48 MB departure scene. `$S` is your output folder
([collaboration.md](collaboration.md#create-your-worktree)); create it with `mkdir -p "$S"`.

| Command | Time | Notes |
|---|---|---|
| `cmake --build build --target vng_make_tunnel_departure vng_scene_probe vng_scene_frames vng_tunnel_scene_tests -j 12` | not timed | Plain `cmake --build build` never rebuilds the tools. Use `-j 8` if other agents are building. |
| `./build/vng_make_tunnel_departure examples/assets "$S/dep_v1.vscene"` | ~4 s | Refuses an existing path: `Scene destination already exists; confirm replacement before Save As`, exit 1. The directory must exist, else `Cannot resolve scene directory`. |
| `./build/vng_scene_probe SCENE --size` | ~20 s | File and decoded size against 64 MiB, instances, tracks, keys, longest track |
| `./build/vng_scene_probe SCENE --list` | ~3.5 s | id, kind, name and blueprint per instance |
| `./build/vng_scene_probe SCENE ID T [T...]` | ~3.5 s | Camera eye, forward and zoom, plus the subject's origin: distance and screen px on 1280x800, or `behind the camera`/`hidden` |
| `./build/vng_scene_frames SCENE NEW_DIR 960 600 --range FROM TO 60` | ~7.5 s load + ~0.04–0.05 s/frame | Hidden window, but needs `DISPLAY` and GL 4.6 (exits 77 without). Never overwrites a PNG. |
| `ctest --test-dir build -R '^vng_tunnel_scene_tests$'` | ~12 s | Authors the departure and checks it (CPU) |
| `ctest --test-dir build -R '^vng_tunnel_render_tests$'` | ~11 s | GPU: frames in every location. Skipped (77) without GL. |

- A full 116 s at 60 fps is ~7,000 frames, 4 to 6 min. That exceeds the default 2 min tool timeout, so render only
  the span you changed, or run it in the background.
- The probe parses times with `strtof` and does not clamp them. `abc` becomes 0; times past the end hold the last key.
- Neither tool has `--help`. Run it with no arguments to print the usage.

## 4. Recipe: add a new scene generator

The example below is `my_shot`: a ship flying along −Z, a chase camera, and a cut at 10 s. Its C++ (generator, tool
and a test built the way step 5 describes) was compiled with the project flags, linked against `./build`'s libraries
and run, and it probes, renders and passes. CMake lines of exactly this shape were configured and built on 2026-09-28
in a CPU-only scratch tree: from a fresh build dir, `vng_make_<name>` plus `vng_<name>_tests` took about 30 s at `-j 8`.
`vng_scene_probe`, `vng_scene_frames` and the GPU tests also pull in the GL runtime (not timed), so run that first
build in the background or with a long timeout. The shot needs the `KeyBatch` rotation fix (the Precondition's
check; see the first trap in §6), or its camera points straight up.

**1. Header** `examples/scenes/my_shot.hpp`. Put the IDs, the duration and the beat times that tests and the demo need
here, as `inline constexpr`.

```cpp
#pragma once
#include "../editor/project.hpp"

namespace example::my_shot {
inline constexpr vng::f32 duration = 20, cut = 10;
inline constexpr vng::u32 hero = 1, camera = 2;   // stable: tests and --analyze evidence use them
[[nodiscard]] vng::content::Result<editor_example::State> author_scene(const std::filesystem::path& assets);
}
```

**2. Generator** `examples/scenes/my_shot.cpp`. Include other example directories only as relative `"../x/..."`
paths. The layering test forbids `examples/support` → `../editor/` or `../scenes/`, and `examples/editor` → `../scenes/`.

```cpp
#include "my_shot.hpp"
#include "key_batch.hpp"
#include <stdexcept>

namespace example::my_shot {
namespace {
using namespace vng;
namespace project = editor_example;
template<class T> T take(content::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}
void checked(content::Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
} // namespace

content::Result<project::State> author_scene(const std::filesystem::path& assets) try {
    // document.mesh (blueprint 1) is required; the departure uses the cube as a placeholder.
    project::State state{.document = {.mesh = take(editor::EditableMesh::load(assets / "colored_cube.vmesh"))}};
    auto& document = state.document;
    document.instances.clear();              // the default document holds "Mesh" (1) and "Sun" (2)
    document.timeline_duration = duration;   // before keying: keys outside [0, duration] fail validation
    constexpr auto ship = static_cast<project::BlueprintId>(3);
    document.mesh_assets.push_back({ship, "SHIP", take(editor::EditableMesh::load(assets / "spaceship.vmesh")), {}});
    document.next_blueprint_id = 4;
    document.instances.push_back({hero, ship, "SHIP / hero", project::MeshSettings{}, {}});
    const auto chase = [](f32 t) { return Look{{3, 2, 14 - 2 * t}, {0, 0, -2 * t}, 1}; };
    const auto side = [](f32 t) { return Look{{25, 1, -20}, {0, 0, -2 * t}, 2}; };
    project::SceneInstance lens{camera, project::BlueprintId::camera, "Camera / main",
                                project::CameraSettings{.active = true}, {}};
    project::place_camera(lens, orbit_pose(chase(0)));   // base transform = the first frame
    document.instances.push_back(std::move(lens));
    document.next_instance_id = camera + 1;

    KeyBatch keys;
    for (f32 t = 0; t <= duration; t += .125F) {
        keys.key({hero, "position"}, t, Vec3{0, 0, -2 * t});
        if (t < cut) key_look(keys, camera, t, chase(t));
        else if (t > cut) key_look(keys, camera, t, side(t));
    }
    key_look(keys, camera, cut - .001F, chase(cut - .001F));                    // outgoing shot runs to the cut
    key_look(keys, camera, cut, side(cut), vng::timeline::Interpolation::hold); // the cut
    keys.simplify({hero, "position"}, .0001F);
    keys.simplify({camera, "position"}, .0001F);
    checked(keys.commit(state));

    document.keyframe_names = {{0.F, "01 / Chase"}, {cut, "02 / Side"}};
    document.environment = {.stars = 4000, .star_seed = 7, .exposure = .95F, .bloom_threshold = 1.1F,
                            .bloom_strength = .3F};   // add .view_distance if anything that must be drawn in frame
                                                      // is > max(200, 4*focus) away (suns light meshes regardless)
    document.world_bounds = {{-50, -50, -60}, {50, 50, 20}};
    state.viewport.mode = project::ViewMode::scene;
    state.viewport.selected_object = hero;
    state.viewport.editor_camera = orbit_pose(chase(0));
    checked(project::validate_animation(state));
    checked(project::validate_active_cameras(state));
    return state;
} catch (const std::exception& error) {
    content::Diagnostic diagnostic;
    diagnostic.message = error.what();
    return std::unexpected(std::move(diagnostic));
}
}
```

- Mesh blueprints take ids 3 and up. Keep `next_blueprint_id` and `next_instance_id` past the last one you use.
- Instance `scale` and `axis_scale` must be ≥ 0.05. To make something smaller, scale the vmesh positions before
  `EditableMesh::create`, as `vessel()` in `tunnel_departure.cpp` does.
- **A procedural mesh of your own** (a torus, a panel, a beacon): fill a `content::vmesh::Document` and pass it to
  `editor::EditableMesh::create`, as `document()` in `examples/scenes/tunnel_scene.cpp` does.
  - Set `vertex_count`, `faces` (`gfx::TriangleFace`), `metadata` (`name`, `source/tool`, `units`, optionally
    `render/lighting`) and the columnar `vertex_fields` the renderer reads: `position` and `normal` f32x3, `color/0`
    f32x3 or f32x4, `emission` f32x1. A wrong type silently zeroes `normal` or `emission`
    ([rendering-and-review.md §1](rendering-and-review.md#1-how-a-vscene-becomes-pixels)).
  - Shared vertices with smooth normals and no `edges` are fine: `create` checks only the mesh limits and a float3
    `position` within ±1,000,000.
  - `Builder` and `Material` in `space_assets.cpp` are file-local. To reuse them, add a public recipe to
    `space_assets.hpp`, which `vng_tunnel_scene` already compiles. `Vec3` has no operators
    ([engine.md §7](engine.md#7-gotchas), item 14), so copy the small vector helpers from the top of
    `space_assets.cpp`.
- **Lighting.** `instances.clear()` also removes the default Sun, so every mesh gets the fixed directional light
  (-.35, .8, .65) ([rendering-and-review.md §1](rendering-and-review.md#1-how-a-vscene-becomes-pixels)). For a key
  light from a chosen unit direction `d`, add a far sun as the voyage does:
  `document.instances.push_back({sun_id, project::BlueprintId::sun, "Sun / key light", project::SunSettings{}, {{d.x * 900000, d.y * 900000, d.z * 900000}, {}, 6000}});`.
  The default radius 1.1 × scale 6000 = 6,600 is under the 10,000 cap. In scene mode a visible sun lights every mesh
  whatever the far plane, so an off-frame sun needs no `view_distance`.
- A scene without `KeyBatch` can key with `project::key_property`, `ensure_camera` and `key_camera`, as
  `tunnel_scene.cpp` does. Each call revalidates the whole timeline, so this is quadratic: fine for tens of keys,
  not thousands.

**3. Tool** `examples/tools/make_my_shot.cpp`. Copy `examples/tools/make_tunnel_departure.cpp`, then change the
include, the namespace, the usage string and the printed summary.

**4. CMake.** Add this inside `if(TARGET vng_editor_project)`, after the `vng_make_solar_system_scene` lines. Put the
test in that block's `if(VNG_BUILD_TESTS)`.

```cmake
add_library(vng_my_shot STATIC examples/scenes/my_shot.cpp)
target_link_libraries(vng_my_shot PUBLIC vng_tunnel_scene)   # KeyBatch lives there; without KeyBatch: vng_editor_project
vng_target_defaults(vng_my_shot)
add_executable(vng_make_my_shot EXCLUDE_FROM_ALL examples/tools/make_my_shot.cpp)
target_link_libraries(vng_make_my_shot PRIVATE vng_my_shot)
vng_target_defaults(vng_make_my_shot)
# inside if(VNG_BUILD_TESTS):
add_executable(vng_my_shot_tests tests/examples/my_shot_tests.cpp)
target_link_libraries(vng_my_shot_tests PRIVATE vng_my_shot Catch2::Catch2WithMain)
target_compile_definitions(vng_my_shot_tests PRIVATE VNG_MY_SHOT_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets")
vng_target_defaults(vng_my_shot_tests)
add_test(NAME vng_my_shot_tests COMMAND vng_my_shot_tests)
```

- `KeyBatch` is compiled only into `vng_tunnel_scene`, which also pulls in the tunnel, voyage, asteroid and fleet
  scenes. For a lighter dependency, move `examples/scenes/key_batch.cpp` into its own small library that
  `vng_tunnel_scene` and yours both link. Never compile it into two libraries.
- `vng_target_defaults` adds `-Wall -Wextra -Wpedantic -Wconversion`. Use `F` literals and `static_cast<f32>`.

**5. Test** `tests/examples/my_shot_tests.cpp`. Include as `"scenes/my_shot.hpp"`, tag `[example][my_shot]`, and call
`author_scene(VNG_MY_SHOT_ASSETS)`. At minimum, check that the subject is in frame at key times and that it does not
shake. Copy the `pixel` lambda and the shake loop from the "voyage flies nose first" case in
`tests/examples/tunnel_scene_tests.cpp`, and project with `render_camera` (§7).

**6. Optional demo** `examples/my_shot.cpp`. It opens a visible window, so it is for the owner rather than for your
review. Copy `examples/tunnel.cpp`: `run_scene_demo(argc, argv, {"vng_my_shot_demo", VNG_MY_SHOT_SCENE_PATH, "TITLE",
capture_time, evidence})`. In the `if(TARGET vng_editor_preview)` block, add an `add_executable` linked `PRIVATE` to
`vng_scene_demo_support`, with
`target_compile_definitions(... PRIVATE VNG_MY_SHOT_SCENE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/my_shot.vscene")`
and `vng_target_defaults`.

**7. Build, author, review, test:**

```sh
cmake -S . -B build        # required after adding targets, or --target may say "No rule to make target"
cmake --build build --target vng_make_my_shot vng_my_shot_tests vng_scene_probe vng_scene_frames -j 12
mkdir -p "$S"
./build/vng_make_my_shot examples/assets "$S/my_shot_v1.vscene"
./build/vng_scene_probe "$S/my_shot_v1.vscene" --size
./build/vng_scene_probe "$S/my_shot_v1.vscene" 1 0 5 9.999 10 15   # hero on screen? camera forward sane?
./build/vng_scene_frames "$S/my_shot_v1.vscene" "$S/my_shot_v1" 960 600 --range 8 12 60
ctest --test-dir build -R '^vng_my_shot_tests$' --output-on-failure
./build/vng_make_my_shot examples/assets examples/assets/my_shot.vscene      # only once the shot is approved
```

Outputs are never overwritten, so bump `_v1` → `_v2` on every rerun. Look at the frames yourself, then follow the
review protocol in [AGENTS.md §8](../../AGENTS.md#8-visual-and-cinematic-work): contact sheet, 60 fps MP4 plus a
1/3-speed copy, reviewer agents, and variants for unclear choices.

**8. Docs.** Add `docs/my_shot.md`, modelled on [docs/solar_system.md](../solar_system.md): what the shot is, how to run
and edit it, the source, the regeneration command and the tests.
- Link it from `README.md` in a paragraph after the `solar_system` one ("See [leaving home]..."). Without a demo, show
  the `vng_make_my_shot` and `vng_scene_frames` commands there.
- Link it from `examples/README.md` too. That file lists `examples/*.cpp` demos, so without a demo add a bullet after
  the `solar_system.cpp` one naming `scenes/my_shot.cpp` and `tools/make_my_shot.cpp` (a proposed convention, not yet
  confirmed with the owner).
- If you commit the `.vscene`, add its row to [assets.md §1](assets.md#1-inventory) and the tool to the scene-tool row
  of [assets.md §3](assets.md#3-tools-what-they-write-and-whether-they-overwrite).

## 5. Techniques

### 5.1 KeyBatch rules

```cpp
example::KeyBatch keys;
keys.key({id, "position"}, t, Vec3{...});                   // incoming defaults to linear
keys.key({id, "visible"}, t, false, Interpolation::hold);    // bools are forced to hold on commit anyway
example::key_look(keys, camera_id, t, {eye, target, zoom}, cut ? Interpolation::hold : Interpolation::linear);
keys.simplify({id, "position"}, .0001F);                     // km units: 10 cm
if (auto ok = keys.commit(state); !ok) ...                   // all or nothing: state is unchanged on failure
```

- **Same time, last call wins.** A key at the same f32 time replaces the earlier one, including its `incoming`. If a
  base-rate sample lands on a cut, it can replace the cut's hold. The voyage's `Timing::sorted` snaps samples within
  1 µs of a cut to exactly the cut and de-duplicates them.
- **Zero seeding.** When a track's first key is after 0, `commit` inserts `{0, base value, hold}`. The base value is the
  instance's transform or settings *at commit time*, so set base transforms before you commit. For objects that appear
  later, create them with `MeshSettings{.visible = false}` and key `visible` true when they enter.
- **Merging.** `commit` merges into existing tracks, so several batches or `key_property` calls can combine.
- **Rotation unwrapping.** For every `rotation` track, `commit` wraps each linear key to within half a turn of the
  previous one, then shifts each component by the fewest whole turns that fit it in ±360: none when it already fits
  (with the §6 fix). A component that spans more than 720° fails with `turns more than
  twice around`; one that spans less but no whole-turn shift fits inside ±360 fails with `spans`. Spinning objects
  therefore get at most two turns in a scene, and keys less than half a turn apart: see the belt rocks' "Tumbling"
  comment in `departure_voyage.cpp`.
- **simplify.** It works on one f32 or Vec3 track only. It keeps hold keys and the key just before each one. It runs
  before the rotation unwrap, so it is conservative across ±180° wraps. The departure uses `position` .0001 (km),
  `rotation` .004°, `zoom` .0005 and `focus` .05.
- **Budget.** Keys ≈ seconds × rate × tracks. `key_look` writes 4 tracks (`position`, `rotation`, `focus`, `zoom`);
  a posed craft writes 2. Over 16,384, `commit` fails and names the six largest tracks. Current usage is in
  [status.md](status.md).

### 5.2 Cuts via hold keys

- A key's `incoming` governs the segment that *arrives* at it. `hold` keeps the previous value until exactly T and
  switches at T (`Timeline::sample`).
- To cut at T:
  1. Key the outgoing shot at T − 0.001 (linear) so it keeps moving up to the cut. Without that key, the old shot
     freezes from its last sample until T.
  2. Key the incoming shot at T with `hold` on every camera track. `key_look(..., Interpolation::hold)` does all four.
- The voyage does this with `Timing::cut`, and uses `hold` in `key_look` whenever `t` is in `cuts`
  (`departure_voyage.cpp`, the bake loop at the end of `author`).
- Anything relocated at a cut (staged bodies, a cheated path, the sun) gets a hold key at the cut. Anything that moves
  up to the cut (the courier) also needs the pre-cut linear key.
- Prefer one camera with hold cuts, as the departure does. Several cameras are also allowed if at most one has
  `active` true at any keyed time (`validate_active_cameras`). With none active, the first camera is used.

### 5.3 Cameras

- The camera instance's `position` is the eye. Its `rotation` is `{-pitch, yaw, 0}` in degrees, where `pitch` is the
  orbit pose's elevation of the eye above its pivot, so a positive `rotation.x` looks up. It looks along local −Z.
  Roll is stored but never rendered. `focus` is the orbit-pivot distance, and `zoom` narrows the 43° vertical
  FOV (`2*atan(tan(21.5°)/zoom)`).
- Always key the eye directly with `key_look`. `render_camera` overrides the pose's eye with the keyed position. An eye
  rebuilt from a distant pivot (`key_camera`/orbit poses) is rounded by metres and shows as shake.
- Pitch is clamped to ±89.5°, so the camera cannot look straight up or down. A look target directly above or below
  the eye loses its framing.
- Far plane in demos and tools: `environment.view_distance` if > 0 (max 4,000,000), else `max(200, 4*focus)`. Near
  plane: `min(0.05, focus*0.25)`. Set `view_distance` when anything visible is farther than 4× the focus (the
  departure uses 1,200,000).
- The editor ignores `view_distance` and uses its **Maximum viewing distance** setting (default 10,000), so distant
  bodies can vanish there but not in the demo. See [rendering-and-review.md](rendering-and-review.md).
- Chase framing: attach eye and target to the craft (`relative(pose, {right, up, back})` in `departure_voyage.cpp`),
  but take "up" from the local vertical, not the craft's banked up. The camera cannot roll, and a pitching frame
  reads as nodding.
- Moves that orbit a subject: slerp the eye around the subject and slerp the look direction separately, rather than
  lerping eye and target (`orbit()` in `departure_voyage.cpp`, which never passes through the subject and turns the
  look evenly).

### 5.4 Dense sampling for fast motion

- Linear keys at the 1/8 s base rate cut corners on curves (chords). Under strong acceleration they also change speed
  in steps at each key. Either one shows as a kink in screen motion, which reads as shake.
- Camera rotation interpolates per Euler component between keys, which is not a slerp. Sparse keys through a turn
  wobble.
- The departure's densities:

  | Use | Rate |
  |---|---|
  | Default | 1/8 s (`Timing::every(handoff.time, duration, .125)`) |
  | Slower moves (under the catcher, after the courier's jump) | 1/15 s |
  | Camera moves, climbs, belt weaving | 1/30 s |
  | Tunnel surge, catch-up, fastest whips | 1/60 s |

  They are declared per location as `Location::dense = {{begin, end, step}}`, and as `sample_times` windows in
  `tunnel_departure.cpp`.
- Key the craft and the camera at the **same** sample times, or their relative motion jitters between keys.
- Integrate paths finely (the voyage's `Flight` uses 1/480 s steps and the belt run 1/240 s), then sample the result.
  Never key a coarse integration directly.
- Compute sample times in double and snap near-cut samples to the cut (`Timing`). Accumulating `t += 1.F/60` in f32
  drifts.
- Pay for density with `simplify`. Straight runs and static shots collapse to a few keys.
- **Orientation inherits the path's second derivative.** Where a craft's forward is a finite difference of its
  position (the voyage's `heading`), a C1 blend in a position term becomes a one-frame kink in the turn rate.
  `ease()` in `departure_voyage.cpp` is a smoothstep, so it is only C1. The Moon pass's
  `ease(t, climb_begin-1, climb_begin+1.5)` in `terrain` starts at 64.5 s and flips the courier's pitch rate by ~7°/s
  in one frame (0.069 °/frame², against at most 0.0076 for the camera there; measured with the rotation check in
  [rendering-and-review.md §3.5](rendering-and-review.md#35-motion-and-shake-checks-script-them)). For position terms
  that drive a heading, blend with a quintic `u³(10 − 15u + 6u²)`, which is C2.
- **Know the key rate of the span you judge.** Anything outside every `Location::dense` window is keyed at 1/8 s. The
  Moon pass has no window for 59–64.2 s, and the courier's other turn-rate kinks there sit exactly on key times.

### 5.5 Staging for 32-bit precision

- The f32 spacing grows with distance from the origin: 1.5 cm at 128 km, 49 cm at 6,530 km, 7.8 m
  at 110,000 km (full table in [limits-and-non-features.md](limits-and-non-features.md#precision-soft-limits-nothing-fails-the-result-just-degrades)).
  Close shots far from the origin jitter visibly. The fix is to move the world, not the camera.
- The voyage's `Stage` maps a location's world point `pivot` to a scene `anchor` near the origin, rotated. It has
  `Stage::facing(pivot, anchor, forward, up)`, `Stage::lit(pivot, anchor, sun_scene)`, `point()`, `direction()` and
  `rotation()`. The Moon and belt locations are shown at `{0, 0, -600}`.
- Bodies that persist across locations (Earth, Moon, base, sun) are re-placed at t = 0 and at the location cuts,
  with hold keys. The cut hides the jump. The sun also moves for relighting (§5.7). Search `departure_voyage.cpp` for
  "Persistent bodies".
- Author in double (`struct V`) and round once with `to()` when keying.
- The sun is a point light, and lighting uses the nearest *visible* sun to each instance's origin. Place it far along
  a fixed direction so it reads as infinitely far. The voyage uses `anchor + direction * 900000`, under the ±1e6
  coordinate limit, with scale 6000. With no visible sun, a fixed directional light takes over.
- In the departure, keep staged action out of positive Z near the tunnel axis. The `tunnel_departure*` lighting styles
  and the portal haze assume that geometry. This is why the anchor is at z = −600.
- Time is f32 too, but at 116 s the spacing is 7.6 µs, which is harmless.

### 5.6 Orientation and gimbal lock

- Instances rotate as `T * Rz * Ry * Rx * S`, in degrees. Craft point local −Z forward and +Y up. `orientation(forward,
  up)` returns the Euler triple, choosing pitch within ±90° so that yaw turns continuously.
- Headings along scene ±X are gimbal lock. `orientation` handles the exact case (roll 0). A **banked** craft whose
  heading crosses ±X still jumps between Euler branches from one sample to the next: with a 30° bank, 0.2° of heading
  takes it from (89.8, −60, −89.8) to (−89.8, −120, 89.8). The per-component unwrap cannot repair that, and linear
  keys across the jump spin the craft. The fleet once flew backwards and upside down this way.
- `forward` parallel to `up` gives NaN, which `commit` rejects as `Non-finite 'rotation' key`.
- The fix is to lay routes out so craft never head along ±X. The voyage rotates each `Stage`, and the belt fleet
  heads on bearing −60°. The test checks nose-first flight with `rotation_math::direction(rotation, {0,0,-1})`.
- Banking: `Flight` banks into sideways acceleration only, smoothed over ~0.3 s and limited to 40°/s. Climbs and
  dives pitch without rolling.

### 5.7 Other techniques in the voyage (`departure_voyage.cpp`)

All of these are file-local, in an anonymous namespace. Copy what you need, or propose extracting them into a shared
header. Don't make a new generator depend on `departure_voyage.cpp`.

| Helper | Use |
|---|---|
| `Flight` | Integrated flight with speed, heading and banking; `at(t)` returns a `Pose {position, forward, up, speed}` |
| `Path` | Catmull-Rom through waypoints, sampled by arc length (its comment says centripetal, but the formula is the uniform one) |
| `Curve` | Quintic Hermite with zero end curvature, so banking never snaps at joins |
| `Location` | One location: `stage`, `courier(t)`, `camera(t)`, `cuts`, `dense`, `relight`, composed sky (`moon`, `earth`), `jumps` |
| `Voyage` | Keying helpers: `add_blueprint`, `add`, `show`, `place`, `turn`, `stretch`, `glow`, `pose` |
| `Timing` | Sample times: base rate, dense windows, cut pairs |
| `moon_pass` terrain following | `floor_at(s)`: per 0.5 km cell, the highest `MoonSurface::height` at −2.5, 0 and +2.5 km across the track; then a running maximum from −4 to +9.5 km along it, then a σ = 8 km Gaussian. `terrain(t)`: that ceiling smoothed over σ = 0.18 s, eased onto `highest_ahead` over 64.5–67 s (`climb_begin` = 65.5). `altitude(t)`: 3.5 km plus the dive in and the climb. `position = center + ground(distance(t)) * (radius + terrain + altitude)`. No test checks clearance above the surface. |

Cheats the voyage uses, each hidden by a cut or a camera move:
- **Relight:** move the sun per shot.
- **Composed sky:** put the Moon or Earth where the frame wants them.
- **Jumps:** move the hero to a cheated path out of sight, and back. **Never difference across a jump.**
  - `moon_pass`'s `heading` and `speed` difference `position` over 0.02 s, and `Flight` differences velocity over
    ±0.02 s to bank. So poses within ~0.02 s of a jump (the Moon pass's `flyby_begin`/`flyby_end`) see it: the
    courier points ~41° off its flight on the 59 s cut ([status.md](status.md) has the numbers), and its roll is
    disturbed for ~0.3 s after.
  - Clamp difference windows to one side of each jump, the way `window()` clamps them to the location's ends.
  - Don't substitute the un-jumped track's heading during the cheated path: the flyby flies ~49° off that track, and
    the nose-first test fails.

Things that go sub-pixel:
- A craft far away gets a glow sprite scaled to its distance, so it stays a steady few-pixel light (`glow_id` in the
  bake loop).
- Needles and sparks use pre-shrunk meshes so their scales stay at or above 0.05.

## 6. Traps

- **Camera looks straight up after `KeyBatch::commit`, on any base without the rotation fix.** A newer base is not
  necessarily fixed: [status.md](status.md) says which branches have it.
  - Without the fix, `commit` adds a whole turn to every rotation component whose keys are all ≤ 0. For meshes this
    is harmless, since Euler angles are periodic. For cameras it is not: `camera_pose`
    (`examples/editor/camera_instances.cpp`) clamps `-rotation.x` to ±89.5 without wrapping it, so a batch-keyed
    camera that only ever looks level or down points straight up. The probe prints `forward (≈0, 1.000, ≈0)` and
    `behind the camera`, and the frames show only stars.
  - The fix shifts a component only when its keys leave ±360, by the fewest whole turns. Check your base with
    `grep -n -A1 'const auto shift' examples/scenes/key_batch.cpp`: the fixed line starts `high > 360.F ?`.
  - Its regression test is "KeyBatch keeps rotations that already fit the editor's range" in
    `tests/examples/tunnel_scene_tests.cpp`: a camera keyed at `rotation.x` −10° and −12° must still look down. Run
    it alone with `./build/vng_tunnel_scene_tests "KeyBatch keeps rotations that already fit the editor's range"`
    (instant, once `vng_tunnel_scene_tests` is built).
- **Shipped scenes are not always generator output.** Some hold editor work that no generator reproduces, and some
  predate camera instances (they use the legacy animation camera, and the probe says `no active camera`).
  - Before any `--replace`, generate into `$S` from the *unmodified* code and compare:
    `cmp "$S/X_v1.vscene" examples/assets/X.vscene`.
  - If they differ, ask the owner before replacing. The current list of hand-edited scenes is in [status.md](status.md).
  - `solar_system` is authored in the editor by design; its generator makes only the starting keyframe.
  - Earth scenes have their own tool and rules: see [assets.md](assets.md).
- **Committed scenes are not rebuilt.** The CPU scene tests call `author_scene` and never read the shipped `.vscene`.
  After changing a generator, the demo shows the old shot until you regenerate with `--replace`.
  - Some GPU tests do load shipped scenes as they are: `vng_editor_runtime_tests` reads `fleet_reveal.vscene` and
    `vng_earth_render_tests` reads `earth_future.vscene`. Run them after replacing those files.
  - Every regeneration adds a multi-MB blob to git history (there is no LFS), so commit a big scene only when its
    content changed, in the same commit as the code change that produced it
    ([assets.md §6](assets.md#6-repository-growth)).
- **Generators compose.**
  - `asteroids::author_scene` and `solar_system::author_scene` start from `fleet::author_scene` and reuse its IDs.
    `solar_system` fails loudly if they move.
  - The departure needs the route `Arabian express / cinematic local` inside `earth_future.vmesh`.
- **Keys outside `[0, timeline_duration]` fail validation.** Set the duration before keying.
- **Unknown `render/lighting` strings silently fall back to standard lighting.** The valid names are in
  [rendering-and-review.md](rendering-and-review.md). The `tunnel` style hard-codes the `tunnel_interior` study's
  geometry and the `tunnel_departure*` styles the departure's (`examples/editor/mesh_shading.hpp`), so don't use them
  elsewhere.
- **Units.**
  - The tunnel and voyage scenes are in km. The fleet, asteroid and solar scenes are unitless.
  - Ship `.vmesh` files are tagged `coordinates/unit = metre`, but `vessel()` in `tunnel_departure.cpp` multiplies
    their positions by 0.010–0.016 into the km scene, so one mesh unit becomes 10–16 m. The courier
    (`spaceship.vmesh`: z −5.05..5.94, x ±3.9) is therefore 0.110 km long and 0.078 km across in the departure.
  - The probe says "km away" whatever the units are.
- **Environment is not animatable.** Stars, exposure, bloom and view distance have no timeline property. Animate a
  sun's `bloom` or a mesh's `brightness` (0–5) instead.
- **Saving.** `SceneFile::save_as` writes atomically and creates new files with mode 0600. Git ignores that mode.

## 7. Testing a cinematic numerically

`tests/examples/tunnel_scene_tests.cpp` is the model. All of it runs on the CPU, and the whole file takes ~12 s.

- **Screen position.** Project a subject's origin with `render_camera(scene, t).snapshot({1280, 800})` and its
  column-major `view_projection[col][row]`. This is what the demo shows. The older `on_screen` helper in the same
  file uses the orbit pose (`p::camera(p::evaluate_camera(...))`); don't copy it for new checks.
- **In frame.** Check a list of times where the subject must be on screen.
- **Large subjects.** The probe and `pixel` see only the origin. For a ring, planet or station, project points on its
  outline and require all of them inside the frame with a margin, and `clip.w > 0` for each. For example, 16 points
  on a ring's outer rim, `(R + r)(cos a, 0, sin a)` in its local frame, transformed by the instance transform, every
  1/60 s.
- **No shake.** In the listed windows only, at 60 fps, `|p(t+1) − 2p(t) + p(t−1)| < 3.5 px`. The windows leave gaps
  ([rendering-and-review.md §3.5](rendering-and-review.md#35-motion-and-shake-checks-script-them)).
- **Nose first.** The velocity direction · `rotation_math::direction(rotation, {0,0,-1})` > 0.9.
- **Coupled transforms.** For example, the Moon and its base share one transform.
- **The windows are hard-coded.** The in-frame times and the shake `{begin, end}` windows are literal lists in the
  voyage test case. A cut moved inside a shake window fails, because a cut is a huge second difference. A removed
  shot leaves a stale in-frame time. Update both lists whenever you move beats, and update the beat table in
  [docs/tunnel.md](../tunnel.md). `keyframe_names` in the code is the source of truth. Not every shot boundary in
  docs/tunnel.md is a cut: 64.3 s is a camera move (the low run starts orbiting to the dawn framing), and
  `keyframe_names` puts "16 / Lunar sunrise" at `climb_begin` = 65.5 s. The cuts are each location's `cuts` plus
  `moon_cut` and `belt_cut`.

## 8. Change a shot of the departure

1. Edit `departure_voyage.cpp`, or `tunnel_departure.cpp` for the skyway act before the handoff. The beat times are
   `inline constexpr` in the two headers, `constexpr` just above each location function in `departure_voyage.cpp`,
   and literals in each location's `cuts`, `dense` and camera code.
2. Build: `cmake --build build --target vng_make_tunnel_departure vng_scene_probe vng_scene_frames vng_tunnel_scene_tests vng_tunnel_render_tests -j 8`.
   If you also changed `examples/editor/mesh_shading.hpp` or the runtime, add the renderer list from
   [rendering-and-review.md §5](rendering-and-review.md#5-add-a-lighting-style) step 5.
3. Author into your output folder `$S` and check it:
   ```sh
   mkdir -p "$S"
   ./build/vng_make_tunnel_departure examples/assets "$S/dep_v2.vscene"
   ./build/vng_scene_probe "$S/dep_v2.vscene" --size                 # keys and MiB against the budget
   ./build/vng_scene_probe "$S/dep_v2.vscene" 2 55 56 57             # 2 = the courier
   ./build/vng_scene_frames "$S/dep_v2.vscene" "$S/dep_v2" 960 600 --range 54 58 60
   ```
4. Update the test windows (§7). Then run
   `ctest --test-dir build -R '^vng_tunnel_(scene|render)_tests$' --output-on-failure` (~25 s).
5. Review with the owner's protocol ([AGENTS.md §8](../../AGENTS.md#8-visual-and-cinematic-work)). Once the change is
   accepted, replace the committed file:
   `./build/vng_make_tunnel_departure examples/assets examples/assets/tunnel_departure.vscene --replace`.

### Where Earth's parts appear in the departure

`express_route::earth_to_world()` (`examples/support/earth_express_route.hpp`) places Earth so that the tunnel mouth
(52.1°E 22.0°N) is at the scene origin. The courier flies toward −Z, which is east; +X is south, −X is north, and 1° is
about 111 km. To map a part's longitude and latitude to scene km:

```sh
python3 - 53.8 20.5 <<'EOF'   # lon lat of a free skyway end (radius 1.024)
import math, sys, numpy as np
lon, lat = map(math.radians, map(float, sys.argv[1:3]))
def turn(a, t):
    a = np.array(a, float); c, s = math.cos(t), math.sin(t)
    return np.array([e*c + np.cross(a, e)*s + a*a.dot(e)*(1 - c) for e in np.eye(3)]).T
R = turn([0, 0, 1], math.radians(68)) @ turn([0, 1, 0], math.radians(38))
d = np.array([math.sin(lon)*math.cos(lat), math.sin(lat), math.cos(lon)*math.cos(lat)])
print(np.round([0, -6530, 0] + R @ d * 6371 * 1.024, 1))
EOF
```

- The example prints `[169.6 -11.1 -191.9]` (Arabia / southern port); the mouth itself prints about `[0 -6.1 -10.6]`.
- On the committed departure the courier climbs from (0, 12, −118) at 19.4 s to (0, 43, −327) at 21 s. The camera
  looks forward and down at the surface from the mouth exit (~18.6 s) to ~20.6 s, up and away from Earth from ~21 to
  25.8 s, and down again from ~26.2 s to the 30 s cut (`vng_scene_probe SCENE 2 18.6 19.4 20.6 21 23.8 26.2`).
  Anything within about 3° east of the mouth is under the climb.

### The lunar base (`LUNAR / Serenity mining works`)

- **Geometry:** `space::lunar_base(surface, site, rail_heading)` in `examples/support/space_assets.cpp`: domes, tubes,
  pads, towers and sheds, plus a mass driver along the rail heading, lit `illustrated`. Its local frame is
  `up` = site, `east` = the rail heading flattened, `north` = up × east, and `ground(e_km, n_km)` returns the surface
  point. The gateway and its habitat ring come from `gateway_station()` and `habitat_ring()` in the same file.
- **Placement:** `moon_pass()` in `departure_voyage.cpp`. The site is 2.6 km beside the courier's ground track, on the
  side away from Earth, and `MoonSite{site, 9}` flattens the ground within 9 km of it. The base is built in the Moon's
  coordinates, so the probe reports the Moon's centre for it
  ([rendering-and-review.md §3.1](rendering-and-review.md#31-probe-numbers-before-pixels)).
- **Shots:** the fixed Earthrise camera `base_eye` sits 6 km beyond the base and 0.45 km up, looking 9.5° up at zoom
  1.3 (`base_shot`, 55.3–59 s); the courier's cheated flyby runs 56.6–59 s over the camera's shoulder. The low run
  along the rail is 59–64.3 s, about 3.5 km above the smoothed ground.
- **Tests** (`tunnel_scene_tests.cpp`): each of the Moon, base, gateway, habitat ring, `BELT / basalt` and
  `WARP / streak` blueprint names exists exactly once, and the Moon and base transforms are equal at 0, 40, 57 and
  100 s. Keep the name, and keep the geometry in the Moon's coordinates.
