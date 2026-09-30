# Rendering and visual review

Read this when you change how a scene looks (lighting, environment, camera, `examples/editor/runtime.*`,
`mesh_shading.hpp`), when you review or iterate on a shot, or when a render looks different in the editor than in
a demo.

**TL;DR**
- Judge a shot from `vng_scene_frames` output, never from the editor viewport. The tool renders offscreen and matches
  the demos pixel for pixel. The editor uses a different camera and far plane.
- The loop: `vng_scene_probe` (numbers, no GPU), then `vng_scene_frames` into a **new** directory, then a contact
  sheet, then a 60 fps MP4 plus a 1/3-speed copy, then reviewer agents, then variants if the choice is unclear.
- Lighting is chosen per mesh blueprint by the metadata string `render/lighting`. Unknown values silently become
  `standard`.
- Every renderer links the static library `vng_editor_runtime`. After changing it, rebuild every consumer, including
  `vng_scene_frames`, which `cmake --build build` never rebuilds.
- Nothing overwrites an existing output file, so always render into fresh paths. Pass `-nostdin` to ffmpeg: without
  it, an existing output makes ffmpeg wait forever at its "Overwrite? [y/N]" prompt in an agent shell.

## 1. How a `.vscene` becomes pixels

An editor scene is a file that starts with `vscene 1.0` and `editor_project = 4;`. It is a serialized
`editor_example::State`. Everything that draws it goes through `editor_example::Runtime`
(`examples/editor/runtime.{hpp,cpp}`, static library `vng_editor_runtime`):

| Consumer | Camera | Window |
|---|---|---|
| `vng_editor_worker` (the editor's viewport) | editor camera (§2) | hidden while editing (frames go to the editor's viewport); shown during Play |
| `vng_{earth,tunnel,fleet_reveal,asteroid_fleet,solar_system}_demo` (`examples/scenes/scene_demo.cpp`) | scene camera | **visible**, 1280x800 |
| `vng_scene_frames` (`examples/tools/scene_frames.cpp`) | scene camera, or explicit `--look` eyes | hidden |
| `vng_{earth,tunnel,asteroid}_render_tests`, `vng_editor_{runtime,overlay}_tests` | per test | hidden |

`vng_scene_probe` links only `vng_editor_project`. It evaluates the scene and never draws.

`Runtime` owns every GPU resource and borrows the `State`, so rendering never writes the document. One frame is
`Runtime::draw` in `runtime.cpp`; read it top to bottom once. It runs these steps:
1. Sample every instance at `t`.
2. Draw the suns into a linear HDR target with reversed depth. If a sun is visible, soften that image. Only the sun
   is blurred; meshes stay sharp.
3. Draw the stars (scene mode with `stars > 0` only). Their directions are fixed by `star_seed`, they are pixel-sized,
   and they have no parallax.
4. Draw the meshes. Instances are grouped per blueprint into one `BlueprintMeshRenderer::render` call each, which
   makes at most 2 GPU draws (solid and wireframe). Meshes are opaque with `CullMode::none`. There is no frustum
   culling and no LOD, and back faces are shaded with the front normal.
5. Draw the portal haze veil. This only happens when a visible mesh uses a `tunnel_departure*` style and the eye is
   inside the tunnel bore (1 < z < 1000 km).
6. Apply bloom, then exposure and Reinhard tone mapping into an sRGB target.
7. Either `present()` to the window, or read the frame back as RGBA8, top-down.

The lighting inputs for each mesh instance come from `mesh_lighting` inside `Runtime::draw`:
- **Light source.** The light is the **nearest visible sun to the instance origin**, treated as a point light.
  - With no visible sun, a fixed directional light (-.35, .8, .65) is used instead. Keying the only sun invisible
    therefore relights every mesh.
  - Mesh view (blueprint inspection) uses a light at the camera.
- **Brightness and glow.** The output is `hdr = (lit surface + color * emission) * brightness`.
  - Mesh `brightness` is animatable, in [0, 5].
  - Any HDR pixel whose brightest channel exceeds `bloom_threshold` (before exposure) glows, whether it is lit or
    emissive.
- **Vertex streams.** `cpu_mesh` in `mesh_shading.hpp` feeds the GPU `position`, `color/0`, `normal` and `emission`.
  - A missing `color/0`, or one that is not f32x3/f32x4, becomes white.
  - A missing `normal` or `emission`, or one whose type is not f32x3 or f32x1 respectively, becomes 0 with no warning.
    An f32x3 `emission` (written like `color/0`) therefore loses all glow, and a wrong-typed `normal` makes the mesh
    unlit.
  - **A zero normal means unlit**: the surface term is the raw vertex color. Emission, brightness and haze still
    apply.
  - Other vertex fields (e.g. `earth/layer`) never reach the GPU.
- **What does not exist.** There are no shadows, no textures and no mesh transparency. See
  [limits-and-non-features.md](limits-and-non-features.md).

Existing docs cover the rest:
- the Runtime ownership tree: [earth.md](../earth.md) (search "MeshPrograms");
- batching: [editor_boundaries.md](../editor_boundaries.md) ("Blueprint rendering batches");
- tunnel haze and the veil: [tunnel.md](../tunnel.md);
- engine bloom: [bloom.md](../bloom.md).

## 2. The two camera paths

If something "looks different in the editor", this is almost always the reason.

| | Scene camera (the shot) | Editor camera |
|---|---|---|
| Used by | the demos, `vng_scene_frames`, `vng_scene_probe`, and the editor during Play until you navigate | the editor viewport when not playing (pilot mode aside, which the app does not enable) |
| Code | `render_camera(state, t, maximum_distance = 0)` in `examples/editor/animation.cpp` | `camera(pose, mode, maximum_distance)` in `examples/editor/project.cpp`, fed the navigation pose |
| Eye | the active camera instance's own keyed position | rebuilt from an orbit pivot: `target + direction * distance` |
| Far plane | `environment.view_distance`, or `max(200, 4 * focus)` when that is 0 | the editor preference "maximum viewing distance" (default 10000); Play uses it too |

What follows from the table:
- **The editor never uses `view_distance`.** In a km-scale scene with a large `view_distance`, such as the departure,
  the editor clips the distant sun, Earth and Moon unless the preference is raised.
- **The editor can show shake that is not in the shot, but only when it is not following the shot.** Play in scene
  mode renders `render_camera`, the keyed eye, until you navigate (`follows_shot` in `examples/editor_worker.cpp`).
  Scrubbing with the pilot camera off, after navigating, or while the smooth zoom settles uses the orbit pose, whose
  eye is rebuilt from a distant pivot and rounded by metres; that reads as shake around a close subject. Play also
  runs on the wall clock, and a Debug worker drops frames, which reads as judder. When the owner reports shake, ask
  where they saw it, and judge it from a `vng_scene_frames` clip and the checks in §3.5.
- **Two `Runtime` overloads use the editor path.** `Runtime::render(device, state, extent, diagnostic, time)` and
  `Runtime::render_frame(device, state, extent, time)` go through `preview_camera_pose`. For shot-accurate output, copy
  `scene_frames.cpp`:
  1. Set `viewport.mode = ViewMode::scene` and `viewport.pilot_camera = true`.
  2. Pass `render_camera(scene, t)` in a `RenderRequest`.

Scene-camera rules (authoring conventions are in [scenes-and-cinematics.md](scenes-and-cinematics.md)):
- **Active camera.** It is the first camera instance whose `active` samples true at `t`, else the first camera
  instance. At most one camera may be active at a time.
- **Scenes without a camera instance.** `fleet_reveal`, `asteroid_fleet`, `earth`, `earth_savannah` and
  `editor_timeline` have none. They use the document's legacy orbit shot (`document.animation_camera` plus its
  tracks).
  - `vng_scene_frames` renders them.
  - `vng_scene_probe ID T` prints `no active camera` for them.
- **Pose from the instance.** Yaw is `rotation.y`. Pitch is `-rotation.x`, clamped to ±89.5°. **Roll (`rotation.z`)
  is ignored.**
- **Lens.** Zoom 1 gives a 43° vertical FOV and zoom 2 about 22.3°. The FOV formula, near plane and zoom and focus
  ranges are in [limits-and-non-features.md](limits-and-non-features.md#camera-and-lens).

## 3. Review workflow

Before you start:
- **Check the tools exist.** They are only on bases that contain `examples/tools/scene_frames.cpp`. See
  [status.md](status.md) for which branches have them.
- **Build them by name.** They are EXCLUDE_FROM_ALL, so `cmake --build build` never refreshes them, and stale binaries
  render with the old shaders. Run this before every review session; it is a no-op when they are up to date:
  `cmake --build build --target vng_scene_frames vng_scene_probe -j 12`. After a runtime change it relinks large Debug
  binaries, so give the call a long timeout ([build-and-test.md](build-and-test.md#long-commands-and-tool-timeouts)).
- **Have a display.** `vng_scene_frames` needs an X display and OpenGL 4.6 even though its window is hidden. On the
  owner's machine agent shells already have `DISPLAY=:1` (there is no xvfb); if it is unset, prefix the command with
  `DISPLAY=:1`. Without a display the tool exits 77.
- **Write renders into your output folder** `$S` ([collaboration.md](collaboration.md#create-your-worktree)), never
  into the repo or under `/tmp`.
- **Set variables in the same call that uses them.** Tool shells do not keep variables between calls, so each block
  below sets its own on its first line. Edit that line.
- **Review generator changes without touching committed assets.** Regenerate the scene into `$S` and point `SCENE`
  at it. For example, `./build/vng_make_tunnel_departure examples/assets "$S/departure_v2.vscene"` takes ~4 s.
  Regenerating a committed `.vscene` needs the owner's OK ([assets.md](assets.md)).

Costs, measured 2026-09-27 on a Debug build with the departure scene:

| Step | Cost |
|---|---|
| `vng_scene_probe --list`, or `ID T...` with any number of times | ~3.5 s, almost all of it scene load |
| `vng_scene_probe --size` | ~20 s (it bisects the parser limit) |
| `vng_scene_frames` | ~7.5 s load, then ~0.04-0.05 s per 960x600 frame (361 frames took 21-25 s); ~0.2 MB per PNG |
| Each ffmpeg sheet or clip below | < 1 s |

Loading dominates the cost, so render everything you need in **one** invocation.
- Beyond ~2,000 frames the render can pass a 2-minute tool timeout. Set a long timeout or run it in the background.
- For a whole-film overview, use a low frame rate and size: `--range 0 116 2` at 480x300.

### 3.1 Probe: numbers before pixels

```sh
SCENE=examples/assets/tunnel_departure.vscene
./build/vng_scene_probe $SCENE --list        # id, kind (mesh|camera|sun|region), instance name, blueprint name
./build/vng_scene_probe $SCENE --size        # file and decoded size vs 64 MiB; keys vs 16,384 total and 4,096 per track
./build/vng_scene_probe $SCENE 2 60 60.5 61  # per time: camera eye, forward, zoom; instance 2's position, distance, screen px
```

How to read the `ID T...` output:
- **Screen coordinates** are for a **1280x800** frame, whatever size you render. For 960x600, multiply by 0.75.
  Anything outside 0..1280 or 0..800 is off-frame.
- **`behind the camera`** means the x/y values are meaningless.
- **`hidden`** means the subject's `visible` is false at that time, keyed or not.
- **The position is the instance origin**, not the mesh centre.
  - Instances built in another body's coordinates report that body's origin. The lunar base shares the Moon's
    transform, so probing it prints the Moon's centre (at 57 s: about 1,734 km away, `behind the camera`). Find
    such subjects by rendering, or aim `--look` at the location's stage anchor (the Moon location's is
    `{0, 0, -600}`).
- **One ID per call.** Every number after the ID is a time: `vng_scene_probe SCENE 1402 57 1401` samples t = 1401.
- **The distance is always labelled "km"**, whatever the scene's units.

### 3.2 Render frames

The command is `vng_scene_frames SCENE OUT_DIR W H ...`. It loads the scene once and has three modes:

| Mode | Arguments | Use it for |
|---|---|---|
| times | `T1 T2 ...` | key moments, and full-resolution frames for reviewers (e.g. `1920 1200`) |
| range | `--range FROM TO FPS` (TO is inclusive) | motion. Use 60 for real time, or 180 when you also want a 1/3-speed clip |
| look | `--look T EX EY EZ TX TY TZ ZOOM FAR`, 9 numbers per frame, repeatable | an alternative camera without editing the scene. Take the target from the probe's subject position |

- **File names** are `f#####_tSSS.SSS.png`. The index follows argument order, which is time order only for `--range`.
- **Fresh directories only.** OUT_DIR is created if needed. If a file of the same name already exists, the tool
  fails with `(existing files are not overwritten): File exists` and exit 1, after the ~7.5 s load. Use a new directory
  per iteration (`shot_v1`, `shot_v2`, ...).
- **`--look` planes.** Near is fixed at 0.05, and FAR must be greater than that, or the run fails after loading.
- **No argument validation.** A non-numeric time becomes 0. Times after the last key hold it. Negative times (and
  times above 86400) sample no keys, so every instance shows its unkeyed base values.
- **Size limits.** Each side must be ≤ 8192 and the frame ≤ 16 Mi pixels, or you get "Invalid preview readback
  extent". Keep W and H even for H.264.
- **Output matches the demo.** `vng_scene_frames ... 1280 800 60` is pixel-identical to
  `vng_tunnel_demo --time 60 --screenshot` (verified 2026-09-27). Editor annotations are never drawn.

### 3.3 Frames, contact sheet and clips in one command

```sh
# Edit this line. SHOT must be a new directory. FPS must be 60 or 180.
S=/abs/path/to/<task>.out SCENE=examples/assets/tunnel_departure.vscene SHOT=$S/surge_v1 FROM=64.4 TO=66.4 FPS=180
./build/vng_scene_frames "$SCENE" "$SHOT" 960 600 --range $FROM $TO $FPS &&
step=$(( ($(ls "$SHOT" | wc -l) + 19) / 20 )) &&
ffmpeg -nostdin -hide_banner -loglevel error -framerate $FPS -pattern_type glob -i "$SHOT/*.png" \
  -vf "select='not(mod(n\,$step))',scale=384:-1,drawtext=text='t=%{pts\:flt\:$FROM}':x=6:y=6:fontsize=14:fontcolor=white:box=1:boxcolor=black@0.6,tile=5x4" \
  -frames:v 1 "$SHOT-sheet.png" &&
ffmpeg -nostdin -hide_banner -loglevel error -framerate 60 -pattern_type glob -i "$SHOT/*.png" \
  -vf "select='not(mod(n\,$((FPS / 60))))',setpts=N/60/TB" -r 60 -c:v libx264 -pix_fmt yuv420p -crf 18 "$SHOT-60fps.mp4" &&
ffmpeg -nostdin -hide_banner -loglevel error -framerate $((FPS / 3)) -pattern_type glob -i "$SHOT/*.png" \
  -r 60 -c:v libx264 -pix_fmt yuv420p -crf 18 "$SHOT-third.mp4"
```

With the values above this takes ~25 s. If an output file already exists, ffmpeg prints "already exists. Exiting."
but still exits 0, so read its output. It produces three files:
- **`$SHOT-sheet.png`**: up to 20 evenly spaced frames in a 5x4 grid, 384 px wide each, each labelled with its scene
  time. The step is rounded up, so some tiles can stay black (361 frames give 19 tiles). Open the sheet with your
  file-read tool rather than reading hundreds of frames.
- **`$SHOT-60fps.mp4`**: real time at 60 fps.
- **`$SHOT-third.mp4`**: 1/3 speed. It is true 60 fps when FPS=180. With FPS=60 it holds each frame three times.

For a render made from a list of times, drop the `drawtext=...,` filter, because its label assumes `--range`. Tile k
is then file number k*step.

### 3.4 Side-by-side variants

```sh
S=/abs/path/to/<task>.out A=$S/surge_v1-60fps.mp4 B=$S/surge_v2-60fps.mp4
ffmpeg -nostdin -hide_banner -loglevel error -i "$A" -i "$B" -filter_complex \
  "[0:v]drawtext=text='A  current':x=12:y=12:fontsize=24:fontcolor=white:box=1:boxcolor=black@0.6[a];[1:v]drawtext=text='B  lower pass':x=12:y=12:fontsize=24:fontcolor=white:box=1:boxcolor=black@0.6[b];[a][b]hstack=inputs=2" \
  -c:v libx264 -pix_fmt yuv420p -crf 18 "$S/compare_ab.mp4"
```

- `hstack` needs inputs of equal height. For three variants, add `-i "$C"` and a `[2:v]...[c]` label, and use
  `hstack=inputs=3`.
- For sheets, give the two PNGs as inputs, use `vstack`, and replace the codec options and output with
  `-frames:v 1 "$S/compare_ab.png"`.

### 3.5 Motion and shake checks: script them

The owner hates camera shake, and that includes nods and whips in the camera's rotation. The tunnel test "The voyage
flies nose first, keeps its courier in frame and never shakes" (`tests/examples/tunnel_scene_tests.cpp`) enforces
this:
- It projects the courier's origin at 1/60 s steps.
- In six hard-coded windows (`{exit_time-1, 29.9}`, `{64.4, 72.4}`, `{75, 78.5}`, `{80.9, 85.85}`, `{97.7, 99.8}`,
  `{100, 110.4}`), it requires the screen-space second difference `|p(t+1) - 2p(t) + p(t-1)|` to stay under 3.5 px.
- It never checks 29.9–64.4 s (the gateway through the Moon's low run), 72.4–75, 78.5–80.9 or 85.85–97.7 s. Nothing
  checks how smoothly the camera or the courier turns (the test's nose-first loop checks only direction). On
  2026-09-28 the shots between the cuts inside 29.9–64.4 s passed the script below anyway (at most 1.53 px); the other
  gaps were not measured. Extend the list over any span you touch, leaving out the cuts.

To spot-check any subject over any span the same way:

```sh
SCENE=examples/assets/tunnel_departure.vscene ID=2 FROM=64.4 TO=72.4
./build/vng_scene_probe $SCENE $ID $(seq $FROM 0.0166667 $TO) | python3 -c '
import re, sys
rows = sys.stdin.read().splitlines()
p = [tuple(map(float, m.groups())) for m in (re.search(r"t= *([-\d.]+).*screen \(([-\d.]+), ([-\d.]+)\)", r) for r in rows) if m]
worst = max((((c[1]-2*b[1]+a[1])**2 + (c[2]-2*b[2]+a[2])**2) ** .5, b[0]) for a, b, c in zip(p, p[1:], p[2:]))
print("max screen second difference %.2f px at t=%.3f (tests allow < 3.5)" % worst)
print(sum("behind" in r or "hidden" in r for r in rows), "samples behind the camera or hidden")'
```

- **Pick spans between cuts.** A jump at a cut is expected.
- **Make it permanent.** Add a check in the same style to the scene's test.

**Check rotation too.** The script above measures only the origin. The probe prints the camera's forward rounded to
3 decimals, too coarse for frame-to-frame checks, and never prints the subject's rotation. Sample any instance's
`rotation` track straight from the file instead (well under a second on the 48 MB departure). Tracks are written as
`{ object = ID; property = "rotation"; ... keys = [`, with one `{ time = T; value = [x,y,z]; incoming = "linear"; },`
per line; the values are Euler degrees applied as T·Rz·Ry·Rx. The script prints the largest change in turn rate,
which is what reads as a nod or a whip:

```sh
SCENE=examples/assets/tunnel_departure.vscene ID=2 FROM=63.5 TO=67   # ID 1001 is the departure's camera
python3 - "$SCENE" $ID $FROM $TO <<'EOF'
import bisect, math, re, sys
path, oid, lo, hi = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
keys, on = [], False
for line in open(path):
    if (m := re.match(r'\s*\{ object = (\d+); property = "(\w+)"', line)):
        on = m.groups() == (oid, "rotation")
    elif on and (m := re.match(r'\s*\{ time = ([^;]+); value = \[([^\]]+)\]; incoming = "(\w+)"', line)):
        keys.append((float(m[1]), [float(v) for v in m[2].split(",")], m[3]))
times = [k[0] for k in keys]
def rotation(t):  # Timeline::sample: each component linear, or held until a hold key
    i = bisect.bisect_right(times, t)
    if i == 0 or i == len(keys) or keys[i][2] == "hold": return keys[max(i - 1, 0)][1]
    (t0, a, _), (t1, b, _) = keys[i - 1], keys[i]
    return [x + (y - x) * (t - t0) / (t1 - t0) for x, y in zip(a, b)]
def turn(r, v):  # T*Rz*Ry*Rx, degrees
    x, y, z = (math.radians(c) for c in r)
    v = (v[0], v[1]*math.cos(x) - v[2]*math.sin(x), v[1]*math.sin(x) + v[2]*math.cos(x))
    v = (v[0]*math.cos(y) + v[2]*math.sin(y), v[1], -v[0]*math.sin(y) + v[2]*math.cos(y))
    return (v[0]*math.cos(z) - v[1]*math.sin(z), v[0]*math.sin(z) + v[1]*math.cos(z), v[2])
def angle(a, b): return math.degrees(math.acos(max(-1., min(1., sum(p*q for p, q in zip(a, b))))))
ts = [lo + k / 60 for k in range(int((hi - lo) * 60) + 1)]
frames = [(turn(r, (0, 0, -1)), turn(r, (0, 1, 0))) for r in map(rotation, ts)]
step = [max(angle(a[0], b[0]), angle(a[1], b[1])) for a, b in zip(frames, frames[1:])]
kink = sorted(((abs(b - a), t) for a, b, t in zip(step, step[1:], ts[1:])), reverse=True)[:3]
print("max turn %.3f deg/frame; largest change in turn rate (deg/frame^2 at t):" % max(step),
      ", ".join("%.4f at %.3f" % k for k in kink))
EOF
```

- On the committed departure (2026-09-28), the camera never exceeds 0.0076 °/frame² between 63.5 and 67 s, while the
  courier kinks at 0.069 at 64.500 s ([scenes-and-cinematics.md §5.4](scenes-and-cinematics.md#54-dense-sampling-for-fast-motion)
  explains why).
- Kinks that land on key times point to sparse keys. Kinks between keys point to the authored motion.
- To make it permanent, sample `rotation_math::direction(rotation, {0,0,-1})` and `{0,1,0}` at 1/60 s in the scene
  test, as its nose-first check does.

### 3.6 Demos: only when you need them

- **The command.** `./build/vng_tunnel_demo examples/assets/tunnel_departure.vscene --time 60 --screenshot NEW.png --analyze NEWDIR`
  opens a **visible** 1280x800 window on the owner's desktop and takes ~8 s.
- **Pass the scene path explicitly.** The default path (`VNG_*_SCENE_PATH`) is absolute and baked in at configure
  time, so a build dir configured from another checkout renders that checkout's asset. The demo prints
  `Shared editor scene: <path>`.
- **`--analyze NEWDIR`** needs a directory that does not exist yet. It writes `composite.png`, one `*-faces.png` per
  subject, and `summary.txt`.
  - The subjects are hard-coded instance IDs in `examples/<name>.cpp` (tunnel: 4 is Earth, 2 is the courier), and
    missing IDs are skipped silently.
  - Each faces image is an isolated re-render of one instance, so an off-screen subject reports 0 covered pixels.
- **`--no-bloom`** zeroes the environment's bloom and every sun's bloom, including keyed tracks, in the loaded copy
  only. Exposure and tone mapping still run.
- **Without `--time`** the demo plays on the wall clock, and a Debug build drops frames. Use that for a feel-check
  only.
- **Option errors print the spaceflight demo's usage line**, because the parser is shared. Ignore the name.

### 3.7 Reviewers, variants, and what to send

- **Reviewer agents** run after each substantial visual iteration (cinematography and editing, art direction and
  lighting, VFX and continuity). Give them the sheets, 2-3 full-resolution key frames, the renderer limits
  ([limits-and-non-features.md](limits-and-non-features.md)) and the commands above so they can render more. The
  brief and the triage rules are in [delegating.md §6](delegating.md#6-reviewer-agents-for-visual-work).
- **Unclear creative choices:** render 2-3 labelled variants (§3.4), recommend one, and keep working on a default
  until the owner picks.
- **What to send, and the owner's taste:** [collaboration.md §6-7](collaboration.md#7-reporting-to-the-owner).

## 4. Lighting styles

A style is set per mesh blueprint, in the mesh document's metadata: the `info { }` block of a `.vmesh`, or
`document.metadata[...]` in a generator. Matching is exact and case-sensitive, and any other value gives `standard`.
The `.vmesh` parser does not interpret these keys.

| Metadata | `LightingStyle` | Look (`shade()` in `examples/editor/mesh_shading.hpp`) | Set by |
|---|---|---|---|
| absent or unknown | `standard` | ambient, sun diffuse, a cool fill, and an albedo-independent specular glint (`sunlight * specular^32 * .22`) | any unlabelled mesh |
| `render/lighting = "illustrated"` | `illustrated` | banded day and highlight, cool shadow, thin cool rim, no specular glint | Earth (`examples/support/earth_assets.cpp`); `lunar_base`, `gateway_station`, `habitat_ring` in `examples/support/space_assets.cpp` |
| `"illustrated"` plus `render/emission = "night"` | `illustrated_night` | illustrated with darker shadows; emission scaled by a smooth night-side factor, so city lights show at night (2.5% by day) | future Earth (`examples/support/earth_infrastructure.cpp`) |
| `"matte"` | `matte` | standard without the glint (bare rock) | `BELT / fragment` (`fragment` in `space_assets.cpp`). In the departure, the three rock blueprints `BELT / basalt`, `BELT / iron` and `BELT / regolith` (`asteroids::rock_mesh(0..2)`, `examples/support/asteroid_assets.cpp`) are styled by one `for (u32 type=0;type<3;++type)` loop in `belt()` in `examples/scenes/departure_voyage.cpp`, which also greys their albedo to 0.44× (that dominates any new look). List their instances with `vng_scene_probe SCENE --list \| grep 'BELT /'` |
| `"regolith"` | `regolith` | illustrated night with the rim × 0.25 (airless ground) | `moon_mesh` in `space_assets.cpp` |
| `"tunnel"` | `tunnel` | standard plus per-pixel km haze `1/(1+d²/18²)` toward warm white, only while the eye is inside the bent study tunnel | `examples/scenes/tunnel_scene.cpp` (`tunnel_interior.vscene`) |
| `"tunnel_departure"` | `tunnel_departure` | standard plus `exp(-d²/18²)` haze, only for world z > 1 with the eye inside the bore (z < 1000), fading as the eye nears z = 0. Also enables the portal veil pass | departure ships: `material()` in `examples/scenes/tunnel_departure.cpp` |
| `"tunnel_departure_night"` | `tunnel_departure_night` | `illustrated_night` shading (not standard) plus the `tunnel_departure` haze; no `render/emission` needed | departure Earth: `author_scene` in `tunnel_departure.cpp` |

- `render/emission` is only read together with `"illustrated"`.
- The tunnel styles hard-code kilometre geometry: Earth radius 6371, a tunnel along +Z with its throat at z = 0, and
  `example::earth::cinematic_tunnel_radius_km`. Don't reuse them for other scenes.
- To see which styles a saved scene uses:
  `grep -o 'render/lighting = \\"[a-z_]*' examples/assets/tunnel_departure.vscene | sort | uniq -c`

## 5. Add a lighting style

1. **Declare it.** In `examples/editor/mesh_shading.hpp`, add an enumerator to `LightingStyle`. Map its metadata
   string in `lighting_style()`, before the final `illustrated`/`standard` return.
2. **Write the look.** In `shade()`, add it as a host-time `if (style == LightingStyle::...)` block.
   - The block chooses which DSL code is emitted; it is not a per-fragment branch.
   - Reuse `normal`, `light`, `eye` and `color`.
   - Leave the three `s.observe(...)` calls and the `s.output(...)` unchanged: the diagnostic capture relies on them.
   - To inherit existing behaviour, add the style to the matching predicate:
     - `night_side`, for night emission and dark shadows;
     - the tunnel-haze conditions;
     - the portal-veil condition in `Runtime::draw`.
   - The standard glint is applied before the style blocks, to every style except `matte`:
     `if (style != LightingStyle::matte) lit = lit + sunlight * (specular32 * .22F);`. A block that assigns `lit`
     afresh, as the illustrated block does, discards it. A block that adds to `lit` keeps it, so add your style to
     that condition if it has its own highlight. `specular32` is the highest precomputed power; square it again for
     a sharper lobe.
   - Float literals take the `F` suffix, and the code must stay `-Wconversion`-clean.
3. **There is nothing to register.**
   - `MeshPrograms::provide()` compiles an instanced and diagnostic program pair on the first use of each style.
     `MeshPrograms` keys its programs by style in a `std::map` (`mesh_programs.hpp`), so a new enumerator needs no
     table entry.
   - `Runtime::update_mesh` re-realizes a blueprint whose style changed.
4. **Opt meshes in.** Add `mesh.metadata["render/lighting"]="<name>";` in the generator, or set it in a `.vmesh`
   `info { }` block. Review with a scene regenerated into `$S` (§3). Regenerating committed assets needs the
   owner's OK.
   - If a generator sets the style, rebuild it (and its scene test) before you author into `$S`. The generators are
     EXCLUDE_FROM_ALL and not in step 5's list, so a stale one writes the old style and your before and after renders
     come out identical. For the departure:
     `cmake --build build -j 8 --target vng_make_tunnel_departure vng_tunnel_scene_tests`; other scenes use their
     `vng_make_*_scene` tool.
   - Check the result: `grep -o 'render/lighting = \\"[a-z_]*' "$S/<scene>.vscene" | sort | uniq -c`.
5. **Rebuild every consumer of `vng_editor_runtime`.** The header is compiled into that static library. The build is
   long, so set a tool timeout:
   ```sh
   cmake --build build -j 12 --target vng_editor_worker vng_scene_frames vng_earth_demo vng_tunnel_demo vng_fleet_reveal_demo vng_asteroid_fleet_demo vng_solar_system_demo vng_earth_render_tests vng_tunnel_render_tests vng_asteroid_render_tests vng_editor_runtime_tests vng_editor_overlay_tests
   ```
6. **Test it.** Add a pixel check modelled on "Earth renders in one instanced batch with matching diagnostic
   geometry" in `tests/examples/earth_render_tests.cpp`:
   1. Change `render/lighting` and rebuild the `EditableMesh`.
   2. Call `runtime->update_mesh(...)` and assert the pixels change.
   3. Restore the style and assert the pixels are identical again.

   For a blueprint in the departure, extend `tests/examples/tunnel_render_tests.cpp` instead, beside "Departure
   renders traffic, the open terminal and Earth without exterior tunnel fog". That case authors the scene with
   `d::author_scene(VNG_TUNNEL_ASSETS)`, renders through the scene camera with
   `runtime->render(*device, {*state, p::render_camera(*state, t), extent, t, false})`, and swaps geometry with
   `++state->document.revision; runtime->update_mesh(*device, *state)`. Find a blueprint with
   `std::ranges::find(state->document.mesh_assets, "BELT / basalt", &p::MeshBlueprint::name)`, copy
   `asset->geometry.document()`, change its metadata and rebuild it with `vng::editor::EditableMesh::create`. Commit
   a test that expects the new style together with the generator change that opts the mesh in, or it fails on the
   commit in between.

   Then run the render tests:
   ```sh
   ctest --test-dir build -R '^vng_(earth|tunnel|asteroid)_render_tests$|^vng_editor_(runtime|overlay)_tests$' -j 5 --output-on-failure
   ```
   `vng_editor_runtime_tests` alone takes ~90 s in Debug, so give the call a 5-minute timeout. Confirm that every
   test says Passed; "Skipped" means there was no GL context.
7. **Review and document it.** Review the result visually (§3). Add the style to the table in §4, and to the lighting
   paragraph of [earth.md](../earth.md) if it is a general-purpose look.

## 6. Environment: stars, exposure, bloom and view distance

Each scene has an `EnvironmentSettings` (`examples/editor/environment.hpp`). Set it in the generator as
`state.document.environment = {...}` (for example in `author_scene` in `examples/scenes/tunnel_departure.cpp`), or in
the `environment = { ... };` line of the `.vscene`.

| Field | Default (no `environment` section) | Allowed (`validate_state`, `project.cpp`) | Effect |
|---|---|---|---|
| `stars` | 0 | ≤ 20000 | number of star sprites; scene mode only |
| `star_seed` | 32 | not checked | star directions |
| `exposure` | 0.9 | [0.01, 10] | applied before Reinhard |
| `bloom_threshold` | 6.5 (almost nothing blooms) | [0, 100] | the HDR level that glows |
| `bloom_strength` | 0 | [0, 1] | the effective value is max(this, the `bloom` of every visible sun) |
| `view_distance` | 0, meaning far = max(200, 4 * focus) | [0, 4,000,000] | the scene camera's far plane; the editor ignores it |

- **None of these is animatable**; there is no timeline property for them. To vary glow over time, key a sun's
  `bloom` or a mesh's `brightness`.
- **Sun size.** A sun's effective radius is `radius * instance scale`. The sun code is `examples/support/sun_*`; see
  [sun.md](../sun.md).

## 7. Gotchas

- **Not every `.vscene` is an editor scene.** `spaceflight`, `solar_flyby` and `custom_scene` are generic documents
  for other demos. Both tools fail on them with `Required property is missing`.
- **Uploads.** `Runtime::create` uploads every blueprint, used or not. After adding a blueprint or replacing geometry,
  call `update_mesh`. Otherwise drawing a new blueprint fails with "Mesh blueprint is not uploaded; update the runtime
  after importing geometry", and replaced geometry silently renders the old mesh.
- **Draw counts.** Only tests and the worker's telemetry see the draw counters (`RuntimeStats::last_mesh_*`); no CLI
  prints them. `tests/editor/mesh_batch_tests.cpp` asserts one draw per blueprint, or two with wireframe.
- **Reversed depth.** Frames use `DepthMapping::reversed`, but compares keep their logical meaning, so write
  `DepthCompare::less` as usual.
- **Jitter far from the origin.** Metre-scale jitter there comes from float precision in transforms, not from depth.
  Stage locations near the origin ([scenes-and-cinematics.md](scenes-and-cinematics.md)).
- **New GL contexts.** Keep `.samples = 0`: MSAA is unsupported and untested. Keep
  `.default_framebuffer_encoding = ColorEncoding::linear`, because `DisplaySurface::copy_to_window` rejects anything
  else.
- **Guide excerpts.** `docs/explore/walkthrough.js` quotes `RenderRequest` and the `BlueprintMeshRenderer` constructor
  and `render` signature verbatim. If you change them without updating the excerpt, `vng_guide_tests` fails
  ([docs-maintenance.md](docs-maintenance.md)).
- **Fast GPU sanity check.** `./build/vng_glfw_opengl_tests "~[modes]"` takes under 1 s, opens no visible window and
  must exit 0 (77 means skipped). Through ctest, its `[modes]` case briefly shows a fullscreen window.
