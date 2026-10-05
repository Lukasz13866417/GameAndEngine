# Assets and the tools that make them

Read this when you add, regenerate, replace or hand-edit anything in `examples/assets/`, or change a mesh recipe in
`examples/support/` or `examples/tools/make_*.cpp`. Also read it before you edit Earth's cities, skyways or hubs.

**TL;DR**
- `examples/assets/` is plain git (no LFS), and most of it is generated text. Several committed `.vscene` files hold
  hand edits that no generator reproduces. Never regenerate or re-save one without the owner's consent
  ([AGENTS.md](../../AGENTS.md)).
- Always generate into a scratch path first and check it with `vng_scene_probe --size`. Replace the committed file with
  `--replace` only after that check and the owner's consent.
- `vng_make_spaceship` and `vng_make_fleet` overwrite their outputs silently. Never point them at `examples/assets`.
- The departure cinematic reads `earth_future.vmesh`, not the Earth inside any scene. To carry an Earth edit into the
  cinematic, export a new `.vmesh`, then regenerate the departure (recipes 4 and 3).
- Current sizes and counts are in [status.md](status.md). This page gives the commands that measure them.

## 1. Inventory

"Build copy" means a demo reads the configure-time copy in `build/examples/assets/`, not the source file (section 5).

"Ask" means: get the owner's consent, and say what a regeneration would lose.

| File | What it is | Made by | Read by | Regenerate? |
|---|---|---|---|---|
| `colored_triangle.vmesh` | 3-vertex mesh | hand-written | `vng_triangle_demo` (build copy) | n/a |
| `colored_cube.vmesh` | 24-vertex cube | hand-written | file_mesh demos and the editor's default mesh (build copies); `document.mesh` of the Earth and departure generators; editor tests | n/a |
| `spaceship.vmesh` | KESTREL ship: metres, -Z forward | `vng_make_spaceship` | spaceflight and solar_flyby demos (build copy); the fleet, tunnel and departure generators; `vng_spaceship_model_tests`; many editor tests; e2e Import | ask. The tool overwrites silently |
| `fleet_{carrier,frigate,escort}.vmesh` | three fleet ships | `vng_make_fleet` (writes all three) | fleet generator, and through it the asteroid and solar-system generators; departure; `vng_spaceship_model_tests` (`fleet_model_tests.cpp`) | ask. The tool overwrites silently |
| `earth.vmesh` | **legacy** Earth with no `editor/blueprint` key | an older `make_earth` that no longer exists | solar-system generator; input to `vng_make_earth --savannah/--future`; Earth tests of the legacy migration path; `vng_editor_ui_tests` (`blueprint_mesh_panel_tests.cpp`) | **never**. The current tool makes a different mesh, and tests need the legacy one |
| `earth_savannah.vmesh` | palette variant | `vng_make_earth SRC OUT --savannah` | e2e (checks it is listed in Import) | ask |
| `earth_future.vmesh` | connected Earth: terrain, clouds and an editable infrastructure catalogue | `--future`, then later passes and edits | departure generator, and through it `vng_tunnel_scene_tests` and `vng_tunnel_render_tests`; input to the `vng_make_earth` modes that read `earth_future.*` | ask. Treat it as authored |
| `earth.vscene`, `earth_savannah.vscene` | turntable scenes: Earth blueprint plus a larger Earth draft | `vng_make_earth` (default / `--savannah`), then the editor | `vng_earth_demo` (`earth.vscene` by default; pass the other as its argument); `earth.vscene` is the input to `vng_make_earth --savannah/--future`; e2e (checks `earth.vscene` is listed in Open) | ask. Hand edits, and no camera instance: `earth.vscene` predates the generator's, and `--savannah` copied it |
| `earth_future.vscene` | Earth, orbit camera, spare Earth; published Earth plus a **diverged** draft | `--future`, then the editor | `vng_earth_render_tests` | ask (section 4) |
| `fleet_reveal.vscene` | 34 s fleet shot | `vng_make_fleet_scene` | `vng_fleet_reveal_demo`; `vng_editor_runtime_tests` | ask. No camera instance: it predates the generator's |
| `asteroid_fleet.vscene` | belt fleet shot | `vng_make_asteroid_scene`, then heavy editor work | `vng_asteroid_fleet_demo`; e2e (checks it is listed in Open) | ask. Hundreds of hand-added instances would be lost |
| `solar_system.vscene` | establishing shot | `vng_make_solar_system_scene` | `vng_solar_system_demo` | ask. The shot is meant to be authored in the editor ([solar_system.md](../solar_system.md)), so treat the file as the owner's work |
| `tunnel_interior.vscene` | 36 s tunnel interior study | `vng_make_tunnel_scene` | `vng_tunnel_demo examples/assets/tunnel_interior.vscene` | ask (unverified whether it is pure generator output) |
| `soldier/soldier.vmesh`, `soldier/soldier.vrig`, `soldier/soldier_pose.vmesh` | the Blender soldier: skinned mesh, armature with the walk, and one static pose | `python3 examples/tools/import_model.py` from the owner's `Soldier.fbx` (not in the repo), with `--colors soldier/colors.json --lights soldier/lights.json --rekey-walk --hole-closer 0.06 --remodel Armor` | `vng_soldier_demo`, `vng_character_tests` (source paths) | ask; needs the FBX. See [characters.md](../characters.md) |
| `tunnel_departure.vscene` | 116 s departure cinematic | `vng_make_tunnel_departure` | `vng_tunnel_demo` (default scene). **No test reads it.** | ask. A regeneration is expected after a change to its generator; first check it is still pure generator output (recipe 2) |
| `editor_timeline.vscene` | `editor_project = 2` test fixture | hand-written | editor tests (`VNG_TIMELINE_SCENE_PATH`) | never re-save it: the editor would write the current format |
| `spaceflight.vscene`, `solar_flyby.vscene` | application documents, **not** editor projects | hand-written | spaceflight and solar_flyby demos (build copies); `vng_spaceflight_render_tests` (both); `vng_spaceflight_scene_tests` (`solar_flyby.vscene` only) | n/a |
| `custom_scene.vscene` | generic structured-document demo data | hand-written | `vng_document_demo` (build copy) | n/a |
| `quiet_sun.veffect` | sun effect preset (`effect_preset = 1`) | hand-written in the editor's preset format (the current export writes 9-digit floats such as `1.29999995`, not `1.3`) | editor Import; e2e (build copy) | n/a |
| `fonts/DejaVuSans.ttf`, `fonts/LICENSE.txt` | vendored third-party font | vendored | text, UI and glow demos and tests; the editor | n/a |

- The `vscene 1.0` header is shared by three dialects: editor projects (`editor_project = N`), application documents,
  and effect presets (`effect_preset = 1`). The editor's scene loader, the scene demos and `vng_scene_probe` accept only
  editor projects. The application documents fail with `Required property is missing`.
- The editor's Open and Import browsers list `<source dir>/examples/assets`, where the source dir is the one baked in at
  configure. The e2e suite fails if `asteroid_fleet.vscene`, `earth.vscene` or `earth_savannah.vmesh` disappear from
  that listing. It never reads their contents: `vng_editor_e2e_tests` authors private fixtures from the generators.
- Before you rename or delete an asset, find every reader with `grep -rn 'NAME' CMakeLists.txt examples tests docs`.
  The guide links some assets (`docs/explore/demo.html` links `colored_cube.vmesh`), so a rename can fail
  `vng_guide_tests`.

## 2. Inspect assets without opening a window

```sh
ls -lS examples/assets                                         # sizes
grep -m1 '^vertices' examples/assets/earth_future.vmesh        # Earth authoring limit 180,224; editor limit 196,608
grep -o 'vertices [0-9]* {\|name = \\"[^\\]*\\"' examples/assets/earth_future.vscene | paste - -   # meshes embedded in a scene
./build/vng_scene_probe examples/assets/fleet_reveal.vscene --list                 # id, kind, instance, blueprint (~1 s)
./build/vng_scene_probe examples/assets/tunnel_departure.vscene --size             # budgets (~20 s in Debug)
./build/vng_scene_probe examples/assets/fleet_reveal.vscene --list | grep -c camera # 0: the scene relies on the legacy camera
```
- `--size` prints the file size, decoded MiB of the editor's 64 MiB, instances, blueprints and tracks, and keys against
  16,384 and 4,096. Its blueprint count leaves out the default `document.mesh`.
- A scene embeds every mesh as an escaped string. An Earth with a draft is therefore stored twice.
- Each `earth/infrastructure/part/<id>` value in a `.vmesh` is one record, written by `write_infrastructure_parts`
  (`examples/support/earth_infrastructure_edit.cpp`). Its fields, in order: kind, lon, lat, end_lon, end_lat, heading,
  size, height, visible, seed, "name", terminal_a, terminal_b, socket_a.part, socket_a.socket, socket_b.part,
  socket_b.socket, altitude_a, altitude_b, altitude, scale, scaffold, scaffold_spacing, the scaffold position count N
  (−1 means automatic) and N positions, tunnel_class, curve_segments, the Bézier control count M (−1 means none) and
  M × (x y z), terminal_incline. To list the hubs:
  `grep -E 'part/[0-9]+ = "3 ' examples/assets/earth_future.vmesh | cut -c1-110`.
- For pictures, use `vng_scene_frames` ([rendering-and-review.md](rendering-and-review.md)). The `vng_*_demo` programs
  open visible windows.
- The tools are `EXCLUDE_FROM_ALL`: `cmake --build build --target vng_scene_probe vng_scene_frames -j 12`.

## 3. Tools: what they write and whether they overwrite

Build any of them with `cmake --build build --target <tool> -j 12`. All need `VNG_BUILD_EXAMPLES`. All except
`vng_make_spaceship` and `vng_make_fleet` also need the editor (`VNG_BUILD_EDITOR`). In the commands below, `$S` is your
output folder ([collaboration.md](collaboration.md#create-your-worktree)), never `examples/` or `/tmp`.

| Tool | Usage | Writes | If the output exists |
|---|---|---|---|
| `vng_make_spaceship` | `OUTPUT.vmesh` | one mesh; prints counts and bounds | **truncates silently**, not atomic (`vmesh::write_vmesh`) |
| `vng_make_fleet` | `OUTPUT_DIRECTORY` (must exist) | `fleet_carrier`, `fleet_frigate`, `fleet_escort.vmesh` | **truncates all three silently** |
| `vng_make_fleet_scene`, `vng_make_asteroid_scene`, `vng_make_solar_system_scene`, `vng_make_tunnel_scene`, `vng_make_tunnel_departure` | `ASSET_DIRECTORY SCENE.vscene [--replace]` | one scene, built from the meshes in `ASSET_DIRECTORY` | refuses with `Scene destination already exists; confirm replacement before Save As` unless `--replace` |
| `vng_make_earth` | `SOURCE_DIR OUTPUT_DIR [MODE]` | a `.vmesh` and a `.vscene` pair (section 4) | refuses unless both are new. `--replace` is accepted only with three modes |

- Scene writes (`SceneFile::save_as`) are atomic: a temp file, then `renameat` (`renameat2` with `RENAME_NOREPLACE`
  for a new file). A replace keeps the old file's mode.
  New files get mode 0600; git still records them as 644.
- The departure's input error names no file. A missing `earth_future.vmesh` prints only `Could not open .vmesh file`.

### Recipe 1: regenerate a mesh made by `vng_make_spaceship` or `vng_make_fleet`
1. Edit `examples/tools/make_spaceship.cpp` or `make_fleet.cpp`. Both tools contain their recipe inline.
2. Write into a new scratch directory: `mkdir -p "$S/fleet" && ./build/vng_make_fleet "$S/fleet"`.
3. Check the face counts with `grep -m1 '^faces' "$S"/fleet/*.vmesh`. The tests require 3,000-12,000 faces for the ship
   and 2,500-6,000 for each fleet ship.
4. With consent, copy the files over `examples/assets/`. Run `cmake -S . -B build` to refresh the build copy of
   `spaceship.vmesh`. Then run `ctest --test-dir build -R '^vng_spaceship_model_tests$'` (about 1 s). It checks both
   guards against the files in `examples/assets`.
5. Every scene built from these meshes is now stale: `fleet_reveal`, `asteroid_fleet`, `solar_system`,
   `tunnel_interior` and `tunnel_departure`. Regenerate only with consent. Most of them hold hand edits (section 1).

### Recipe 2: check whether a committed scene is still pure generator output
Do this **before** you change the generator, so the binary still matches the committed code.
```sh
cmake --build build --target vng_make_tunnel_departure -j 12
./build/vng_make_tunnel_departure examples/assets "$S/td.vscene"     # ~4 s (Debug)
cmp "$S/td.vscene" examples/assets/tunnel_departure.vscene && echo "pure generator output"
```
If they differ, someone edited the committed scene. Stop and ask before you `--replace` it. If you have already changed
the generator, run the same check in a detached worktree at `HEAD`, or ask.

### Recipe 3: regenerate the departure cinematic
Needed after changing `examples/scenes/{tunnel_departure,departure_voyage,key_batch}.cpp`,
`examples/support/space_assets.cpp`, `examples/support/asteroid_assets.cpp` (the voyage's belt rocks),
`earth_future.vmesh` or any ship mesh. The list is not exhaustive: a change to anything the generator links (Earth
support code, editor encoding) can change its output, and recipe 2's `cmp` tells you. Its choreography is in
[scenes-and-cinematics.md](scenes-and-cinematics.md).
```sh
cmake --build build --target vng_make_tunnel_departure vng_scene_probe vng_tunnel_scene_tests vng_tunnel_render_tests -j 12
./build/vng_make_tunnel_departure examples/assets "$S/td-new.vscene"   # ~4 s
./build/vng_scene_probe "$S/td-new.vscene" --size                      # ~20 s; must stay under every budget
# review frames: rendering-and-review.md; then, with consent:
./build/vng_make_tunnel_departure examples/assets examples/assets/tunnel_departure.vscene --replace
ctest --test-dir build -R 'vng_tunnel_(scene|render)_tests' --output-on-failure   # ~25 s; the render test needs a GPU
```
- The tests author their own departure from `examples/assets`. A stale committed `tunnel_departure.vscene` passes every
  test, and only the demo shows it. Regenerate it in the same commit as the code change that caused it (section 6).
- Put the new `--size` line in the commit body. Update [status.md](status.md) if it lists the scene's size.
- **Preview an Earth candidate without touching `examples/assets`.** The generator reads exactly six meshes from
  ASSET_DIRECTORY (`author_scene` in `tunnel_departure.cpp`): `colored_cube`, `spaceship`, `fleet_carrier`,
  `fleet_frigate`, `fleet_escort` and `earth_future.vmesh`. Build a scratch asset directory:
  ```sh
  mkdir -p "$S/assets_v1" && cp examples/assets/{colored_cube,spaceship,fleet_carrier,fleet_frigate,fleet_escort}.vmesh "$S/assets_v1/"
  cp "$S/earth_v1/earth_future.vmesh" "$S/assets_v1/"      # the candidate
  ./build/vng_make_tunnel_departure "$S/assets_v1" "$S/dep_v1.vscene"
  ```
  With the committed `earth_future.vmesh` in place of the candidate, the output is byte-identical to the committed
  departure (checked 2026-09-28).

## 4. Earth

Earth is one ordinary mesh blueprint. Its infrastructure (settlements, skyway tunnels, hubs, terminals, elevators,
joiners, processors) is a recipe catalogue stored in the `.vmesh` metadata as `earth/infrastructure/part/<id>`. The
geometry is baked into the same mesh. Two per-vertex fields record which part owns each vertex:
`earth/infrastructure` (the owner's `InfrastructureKind`; 0 means terrain and clouds) and `earth/infrastructure-part`
(the part id).
- CPU code is in `examples/support/earth_*.{hpp,cpp}`, target `vng_earth_assets`. The editor links it through
  `vng_editor_project`. Scripted network passes are in `examples/scenes/earth_network.cpp` (target `vng_earth_scene`).
- A mesh counts as Earth when `editor/blueprint = "earth"`, or when `source/tool` names
  `examples/support/earth_assets.cpp` (`is_earth` in `earth_edit.cpp`). A copied mesh with neither key loses all Earth
  editing.
- Only terminals and joiners have sockets (`infrastructure_sockets` in `examples/support/earth_connections.cpp`): a
  terminal has 1 (Entrance), a joiner 3 (Inlet, Left outlet, Right outlet). Hubs, settlements, elevators and
  processors have none, so no skyway connects to a hub.
  - The five Arabian hubs stand alone, and each global gateway hub sits 0.45° east and 0.35° north of its corridor's
    B end (`expand_global_infrastructure`). A skyway "between two hubs" is therefore two free ends placed near them.
  - A free end sits at radius `skyway_endpoint_radius + altitude_a/b` (`skyway_endpoint_radius` is 1.024, in
    `earth_skyway.hpp`). Offset each end from its hub as the gateways do, or raise it, and check the result in frames.
- Background and the user-facing workflow: [earth.md](../earth.md). Numeric part limits:
  [limits-and-non-features.md](limits-and-non-features.md#meshes-and-earth).

### `vng_make_earth` modes (`examples/tools/make_earth.cpp`)
| Mode | Reads from SOURCE_DIR | Writes to OUTPUT_DIR | Notes |
|---|---|---|---|
| (none) | `colored_cube.vmesh` | `earth.vmesh`, `earth.vscene` | fresh `make_mesh()`, **not** the committed legacy `earth.vmesh` |
| `--savannah` | `earth.vmesh`, `earth.vscene` | `earth_savannah.*` | palette variant |
| `--future` | `earth.vmesh`, `earth.vscene` | `earth_future.*` | rebuilds all infrastructure, then runs the redesign and global passes; adds camera keys at 0, 45 and 90 s |
| `--refresh-future` | `earth_future.*` | `earth_future.*` | **regenerates every infrastructure vertex**: hand vertex edits are lost, recipes survive |
| `--redesign-infrastructure [--replace]` | `earth_future.*` | `earth_future.*` | no-op when `earth/infrastructure/composition = "desert-express-2"` |
| `--global-infrastructure [--replace]` | `earth_future.*` | `earth_future.*` | no-op when the `earth/infrastructure/global-corridors` key is present |
| `--tunnel-classes [--replace]` | `earth_future.*` | `earth_future.*` | **do not run on the current Earth** (below) |

- OUTPUT_DIR must exist. Without `--replace` it must contain neither output file, so the modes that read and write
  `earth_future.*` need an OUTPUT_DIR other than SOURCE_DIR. With `--replace`, the `.vmesh` is truncated in place
  (not atomic), and the scene is replaced atomically.
- Every mode except the default also rewrites each Earth asset and draft inside the source scene. The written `.vmesh`
  comes from the source `.vmesh`, not from the scene's copy.
- A mode applies its pass to the source `.vmesh`, then to every Earth blueprint and draft in the source `.vscene`,
  before it writes anything. If the pass fails on any of them (for example, because a part it looks up by name is
  missing from the diverged draft), the tool exits 1 and writes neither file.
- The departure reads only `earth_future.vmesh`. `earth_future.vscene` is read by `vng_earth_render_tests`, and by
  `vng_earth_demo` when you pass it as the argument. Ask the owner whether a regenerated `.vscene` should be committed
  too.
- **`--tunnel-classes` is unsafe** (read from code, not run). `author_tunnel_network` (in `earth_scene.cpp`) returns
  early only when `earth/infrastructure/network-revision` is `"bezier-lanes-2"`; the committed Earth has
  `"desert-long-haul-3"`. On that Earth it reassigns every skyway's class in a local/regional/trunk/regional cycle,
  including the express route, which tests require to stay `local`. It also adds 4 feeders, 9 lanes and a second part
  named "Arabian express / cinematic local" (whether that fits the vertex budget is unverified). Check the marker
  first: `grep 'network-revision' examples/assets/earth_future.vmesh`.

### Vertex budget

Earth authoring allows 180,224 vertices (`max_earth_vertices`), below the editor's
196,608 per-mesh limit (`max_mesh_vertices`). Byte budgets still apply independently.
```sh
grep -m1 '^vertices' examples/assets/earth_future.vmesh              # total
grep -o '^    earth/infrastructure/part/[0-9]* = "[0-9]' examples/assets/earth_future.vmesh \
  | awk -F'"' '{print $2}' | sort | uniq -c                          # parts per kind (1 settlement, 2 skyway, 3 hub, ...)
awk '/^vertices /{v=1} /^faces /{v=0} v && /^ *\[/{split($0,f,/[][]/); n[f[12]]++} END{for(k in n) print k, n[k]}' \
  examples/assets/earth_future.vmesh | sort -n                       # vertices per owner kind; f[14] = per part id
```
- The awk columns assume the field order in the file's `fields { }` block: position, normal, `color/0`, emission,
  `earth/layer`, `earth/infrastructure`, `earth/infrastructure-part`. Check that block first.
- Measured part costs in the committed `earth_future.vmesh` (2026-09-28): the express route 7,674 vertices; each of
  its inbound and service lanes 3,842; a 40-segment regional lane with a terminal 3,074; a 40-segment local lane with
  4 supports and no terminal 1,642; a 32-segment global corridor 1,266; an Arabian hub 196; terrain and clouds 37,874.
  Per part, with names (prints `id kind name... vertices`):
  ```sh
  F=examples/assets/earth_future.vmesh
  join <(grep -o '^    earth/infrastructure/part/[0-9]* = "[0-9] [^"]*\\"[^\\]*\\"' $F \
           | sed -E 's/^.*part\/([0-9]+) = "([0-9]) .*\\"([^\\]*)\\"$/\1 \2 \3/' | sort -k1,1) \
       <(awk '/^vertices /{v=1} /^faces /{v=0} v && /^ *\[/{split($0,f,/[][]/); n[f[14]]++} END{for(k in n) print k, n[k]}' $F \
           | sort -k1,1) | sort -n
  ```
- An edit over budget fails with `Earth mesh vertex budget exceeded (N / 180224)...`. Single-part edits patch
  vertices in place only when the part's topology is unchanged.

### Recipe 4: change Earth's infrastructure by hand and carry it into the cinematic
1. Decide with the owner which Earth is canonical. `earth_future.vmesh` and the draft inside `earth_future.vscene` have
   diverged before: different part sets, cloud edits and vertex counts. Compare the embedded vertex counts
   (section 2) and the next part ids:
   ```sh
   grep -o 'earth/infrastructure/next-id = \\*"[0-9]*' examples/assets/earth_future.vscene examples/assets/earth_future.vmesh
   ```
2. Open a **copy** of the scene in the editor ([editor.md](editor.md#3-run-the-editor-safely)). Select Earth, edit
   with the infrastructure controls (`examples/editor/earth_infrastructure_controls.cpp`), and apply.
3. Use **Export new .vmesh** to write a new path. It opens with `O_EXCL` and never overwrites.
4. With consent, move that file over `examples/assets/earth_future.vmesh`. Check the vertex total (above), then run
   `ctest --test-dir build -R '^vng_tunnel_scene_tests$'` (about 12 s), which reads this file.
5. Regenerate the departure (recipe 3). The committed scene does not update itself.
- Keep the express route's name exactly `Arabian express / cinematic local` (`express_route::name`). The departure finds
  the route by that name. If you rename it, regeneration throws `Earth is missing the Arabian express route...`.
- Tests of `departure::flight()` and `velocity()` sample the canonical `express_route::recipe(1)`, not the file's route.
  Reshaping the route changes the scene but not what those tests check.
- `vng_tunnel_scene_tests` reads `earth_future.vmesh` through `VNG_TUNNEL_ASSETS`, which is baked to your checkout's
  `examples/assets`. Its case "Express departure has a continuous close camera and clears the low terminal" checks:
  - more than 70 parts, at least 36 skyways, and at least 5 hubs in 49–57°E, 19–27°N;
  - an express route that is local and Bézier, has an 8° terminal incline, and ends near 52°E 22°N;
  - that no Earth triangle lies between successive courier positions, every 0.25 s until the Moon cut (a failure
    names the part: `Flight obstruction: <name>`);
  - that no Earth triangle lies between the camera eye and the courier.

  The two path checks are the ones a new part near Arabia is most likely to break, and they cannot read a scratch
  Earth. To run them before the visual review, ask the owner to cover a temporary placement of the candidate in your
  own worktree's `examples/assets` in your consent request; undo it with
  `git restore examples/assets/earth_future.vmesh`.

### Recipe 5: add a scripted Earth network pass
1. Write the pass in `examples/scenes/earth_network.cpp` and declare it in `earth_scene.hpp`. Guard it with a metadata
   marker so that it is idempotent, as `expand_global_infrastructure` does.
2. Build parts with the idiom both existing passes use (`earth_network.cpp`):
   `result=add_infrastructure(*result,p.kind,p.location); auto parts=infrastructure_parts(*result); p.id=parts->back().id; p.seed=p.id; result=edit_infrastructure(*result,p);`.
   - For a Bézier lane, copy the control construction from `redesign_infrastructure` (its `control_radius`; the
     global corridors use a slightly higher base radius).
   - Never call the whole-catalogue `rebuild_infrastructure`: it regenerates every infrastructure vertex.
   - `redesign_infrastructure` removes every non-settlement part and erases the `global-corridors` marker. If your
     pass depends on parts the redesign replaces, erase your marker there too.
3. In `examples/tools/make_earth.cpp`, add `const bool yours=argc>=4&&std::string_view(argv[3])=="--your-mode";` and
   OR it into `retune`. That makes the mode read and write `earth_future.*` and accept `--replace`. Route it in the
   `change` lambda **before** the `if(retune)` line, and add it to the usage string.
   - A flag in `retune` that you don't route falls through to `author_tunnel_network`, the unsafe `--tunnel-classes`
     pass.
   - To include the pass in fresh `--future` builds, append it after `expand_global_infrastructure` in the
     `if(result&&!refresh)` chain.
   - Run the mode into a new directory, and check the vertex total.
4. Add a `TEST_CASE` with its own tag to `tests/examples/earth_assets_tests.cpp`, and run only that tag. The whole
   binary takes about 70 s ([tool timeouts](build-and-test.md#long-commands-and-tool-timeouts)). The `[global-network]` tag
   alone takes about 5 s.

### Earth gotchas
- `earth/infrastructure/parts-version` is compared as a **string**, for example `>= "3"`, and checked against an
  explicit `"1"`..`"9"` whitelist in `infrastructure_parts()` (`earth_infrastructure_edit.cpp`). Version `"10"` would
  break every comparison.
- The ≤ 14 Bézier-control limit in `valid()` is coupled to a fixed `std::array<..., 16>` in `Route::sample_world`
  (`tunnel_departure.cpp`). Raise both together.
- A plain Earth with no catalogue behaves differently depending on the call. `infrastructure_parts()` returns the
  **default** catalogue, which `rebuild_infrastructure` then populates. `add_infrastructure` starts an **empty** one.
- `docs/explore/codebase-details.js` quotes the `CloudFormation` struct from `earth_clouds.hpp`. Changing that struct
  fails `vng_guide_tests` ([docs-maintenance.md](docs-maintenance.md)).

## 5. Procedural assets and asset paths
- The voyage's Moon, lunar base, gateway, habitat ring, glow spheres, fragments and plumes live in
  `examples/support/space_assets.{hpp,cpp}`. They are built with a small `Builder` there:
  - `triangle`, `quad`, `box`, `beam` and `prism` are flat-shaded, with 3 vertices per triangle;
  - `dome` emits 4 vertices per quad, with sphere normals;
  - finish with `std::move(b).document(name, parts[, lit])`. `lit = false` zeroes the normals, which the renderer
    draws unlit.
- The builders are declared in `space_assets.hpp`: `moon_mesh`, `lunar_base(surface, site, rail_heading)`,
  `gateway_station` (hub, arms, wings and the mass-driver catcher), `habitat_ring`, plus `glow_sphere`, `fragment` and
  `engine_plume`.
- The palette is the `Material` constants at the top of `space_assets.cpp`, shared with Earth's infrastructure look:
  the greys `hull`, `plating`, `dark`, `solar` and `frame_metal`; the lights `warm_window`, `cyan_light`,
  `white_light`, `red_light`, `green_light` and `amber_light` at emission 2.2–3; and `beacon_red`, `lamp_cyan` and
  `lamp_white` at 4–6 for lamps seen from kilometres away. Reuse them so new parts match.
- To add one, declare it in `space_assets.hpp` and register it with `add_blueprint` in
  `examples/scenes/departure_voyage.cpp`. Set the `render/lighting` metadata if it needs a lighting style other than
  standard ([rendering-and-review.md](rendering-and-review.md)). Then run recipe 3 and watch `--size`.
- `space_assets.cpp` already belongs to `vng_tunnel_scene`. A **new** `.cpp` file must be added to that target in
  `CMakeLists.txt`, followed by `cmake -S . -B build`. New Earth support files go into `vng_earth_assets` the same way.
- There are two asset-path conventions:
  - Editor-scene demos and most tests read the **source tree**, through compile definitions such as
    `VNG_TUNNEL_SCENE_PATH` and `VNG_TUNNEL_ASSETS`. A build dir therefore reads its own checkout's files. The
    generators read the `ASSET_DIRECTORY` you pass them.
  - The triangle, file_mesh, document, text, UI, glow, spaceflight and solar_flyby demos, the editor's default mesh
    and font, and some editor tests read **configure-time copies** in `build/examples/assets/`
    (`configure_file(... COPYONLY)`). The copy refreshes at the next configure: `cmake -S . -B build`, or any
    `cmake --build`, which re-runs configure when a copied source changed. A binary you run without rebuilding
    reads the stale copy.
- A path inside an application document, such as `mesh = "spaceship.vmesh"`, resolves against that document's own
  directory.

## 6. Repository growth
- There is no LFS. Each regenerated large scene adds a new multi-MB blob to history. Git's delta compression shrinks it
  when it repacks, but how much that saves per regeneration is unverified.
- Regenerate and commit a large asset only when its content has to change. Commit it in the same commit as the code
  change that produced it, as a8aff7d did, so every commit's asset matches its generator. Never put it in
  an unrelated commit. Name it in the commit body with its new `--size` line (and the `cmp` result against fresh
  generator output), and never commit scratch outputs.
