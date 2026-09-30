# Build and test

Read this when you set up a build directory, build anything, choose tests for a change, or a
test or binary behaves in a way you don't expect.

**TL;DR**
- Give each worktree its own `build/`. Configure it offline from the main checkout's fetched
  dependency sources, and always pass `-DCMAKE_BUILD_TYPE=Debug` ([§1](#1-configure-a-worktree-build-offline)).
- `ctest` never builds anything. Rebuild the test target, and any `vng_make_*`/`vng_scene_*` tool you
  use, before you run it. Then run the binary directly with a Catch2 filter.
- `node --test tests/layering/layering.test.cjs docs/explore/tests/content.test.cjs` needs no
  build and takes under 1 s. Run it after every change.
- Pick tests from [the table](#5-which-tests-for-which-change). "Skipped" does not mean "passed".
  Anything marked **(long)** (over 60 s) needs an explicit tool timeout or a background run.
- There is no CI, git hook, `-Werror` or formatter. What you ran is the only gate, so report it exactly.

Dated facts, such as tests that fail right now, are in [status.md](status.md), not here.

## 1. Configure a worktree build (offline)

Create the worktree first, as described in [collaboration.md](collaboration.md#create-your-worktree).
Then run this from the worktree's root:

```sh
MAIN="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")"   # the main checkout
D="$MAIN/build/_deps"
ls -d "$D"/{catch2,glad,glfw,harfbuzz}-src          # all four must exist
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DFETCHCONTENT_SOURCE_DIR_CATCH2="$D/catch2-src" \
  -DFETCHCONTENT_SOURCE_DIR_GLAD="$D/glad-src" \
  -DFETCHCONTENT_SOURCE_DIR_GLFW="$D/glfw-src" \
  -DFETCHCONTENT_SOURCE_DIR_HARFBUZZ="$D/harfbuzz-src" \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
```

- It takes about 2 s. The line "WARNHarfBuzz has a Meson port…" is harmless.
- Always pass `-DCMAKE_BUILD_TYPE`. The README's bare `cmake -S . -B build` leaves the build type
  empty, which means no `-g` and no `-O`.
- Your build compiles the dependencies from `$D`. CMake also applies the GLFW scroll patch in
  place in `$D/glfw-src`. The patch step is idempotent, and it refuses modified sources with
  "requires clean GLFW 3.4 sources".
  - Never delete, `git clean` or edit `$MAIN/build/_deps`.
  - If `$D` is missing, a fresh build dir configured without the five `FETCHCONTENT_*` flags
    downloads the pinned tags. That needs the network, so ask the owner first
    ([collaboration.md](collaboration.md#3-approvals)).
- FreeType is taken from the system. Without a system FreeType, CMake fetches it; to stay offline, add
  `-DFETCHCONTENT_SOURCE_DIR_FREETYPE=<src>`.
- Never configure, build or run `ctest` in the main checkout; other agents work there
  ([collaboration.md](collaboration.md)).
- **Disk.** A full Debug `build/` takes about 4.3 GB. One with only `vng_make_tunnel_departure` takes about 0.4 GB, and
  that tool builds from scratch in about 26 s ([delegating.md §2](delegating.md#2-set-up-isolated-worktrees)). The
  disk is shared and free space has been tight (last reading:
  [status.md](status.md#machine-and-agent-environment)). Run `df -h /home` before you configure a new build dir,
  build only the targets you need, and do the full build only for the pre-PR suite.

**Requirements.** All of these are installed on the owner's machine.

| Needed for | Requirement |
|---|---|
| Everything | CMake ≥ 3.25, a C++23 compiler (g++ 14 is what `build/` uses; the installed clang++ 18 with libstdc++ 14 cannot compile the headers, because `std::expected` is unavailable there), libpng dev files |
| The GLAD loader, generated at build time | `python3` with Jinja2 |
| The GLFW patch step | `git` |
| Text | FreeType ≥ 2.10 and HarfBuzz; each is fetched if not found |
| UI and the editor app | ICU ≥ 67 dev files (`libicu-dev`) |
| Editor preview, worker and app | Linux, Threads |
| GLFW | its own X11 and Wayland build dependencies (both backends are built) |
| Scroll tests | X11 with Xi and Xtst; `xvfb-run` or `Xephyr` for `vng_x11_scroll_tests` |
| The two node tests | `node` on PATH **at configure time**; otherwise they silently vanish from CTest |
| GPU tests, demos and review renders | an OpenGL 4.6 context on an X display |

**Options** are at the top of `CMakeLists.txt`:

| Option | Default | Switches |
|---|---|---|
| `VNG_BUILD_OPENGL` | ON | GL backend, all GPU tests and demos |
| `VNG_BUILD_WINDOW_GLFW` | ON | GLFW window, demos, the editor app |
| `VNG_BUILD_TEXT` | ON | Text shaping and rendering. **`vng_ui` is created inside this block.** |
| `VNG_BUILD_UI` | ON | `vng_ui`, `vng_editor_ui`, the editor app (needs ICU) |
| `VNG_BUILD_EDITOR` | ON | Editor libraries, and with them every scene generator |
| `VNG_BUILD_EXAMPLES` | ON | Demos, scene libraries, `vng_make_*`, review tools |
| `VNG_BUILD_TESTS` | ON | All tests |
| `VNG_ENABLE_SANITIZERS` | OFF | ASan and UBSan on vng targets only (not the dependencies) |

- If you pass `-DVNG_BUILD_TEXT=OFF`, also pass `-DVNG_BUILD_UI=OFF`. The editor app checks the
  option, not whether the target exists, so it would link a missing `vng_editor_ui`. This is
  inferred from the CMake code; no such build was run.
- `vng_editor_preview` and its test need only EDITOR, on Linux. The editor runtime, worker and app, the
  scene demos, `vng_scene_frames`, `vng_scene_probe` and the GPU editor tests are also Linux-only, and
  need OPENGL, WINDOW_GLFW, EXAMPLES and EDITOR all on (the app also needs UI). `vng_scene_probe` is
  CPU-only but is declared in that block. The scene libraries and the scene generators need only
  EXAMPLES and EDITOR; the mesh generators `vng_make_spaceship` and `vng_make_fleet` need only EXAMPLES.
- For a CPU-only build, pass `-DVNG_BUILD_OPENGL=OFF -DVNG_BUILD_WINDOW_GLFW=OFF`
  ([README](../../README.md#build-and-test)).
- **Sanitizers:** run the configure command above with `-B build-asan` instead of `-B build`, and
  add `-DVNG_ENABLE_SANITIZERS=ON`. Nobody has checked that a sanitizer build with the GPU and
  editor targets on runs clean. Delete the directory when you are done.

## 2. Build

```sh
cmake --build build -j 12 --target vng_editor_tests      # one target plus what it links
cmake --build build -j 12 --target vng_scene_frames vng_scene_probe vng_make_tunnel_departure
cmake --build build -j 12                                # everything in ALL
cmake --build build --target help                        # list every target
```

- **Parallelism.** Use `-j 12` or less, and `-j 8` when other agents are building. Never use a bare `-j`
  as the README does: with Makefiles it means unlimited jobs, and Debug C++23 compiles can
  exhaust RAM. Never run two builds in the same build directory at once.
- **A from-scratch build of everything takes several minutes** (not timed). Run it in the
  background or with a long tool timeout ([§4](#long-commands-and-tool-timeouts)).
- **Targets that `cmake --build build` never builds.** These are EXCLUDE_FROM_ALL: all
  `vng_make_*` tools, `vng_scene_frames` and `vng_scene_probe`. Name them after every change to code
  they link, or you will run stale binaries.
- **Implicit dependencies.**
  - `vng_editor_app` and `vng_editor_worker_tests` depend on `vng_editor_worker`, and
    `vng_editor_e2e_tests` links `vng_editor_app`.
  - `vng_editor_app` is a static library. The editor executable is `vng_editor_demo`.
- **New targets and tests need a reconfigure: `cmake -S . -B build`.** The cache keeps your flags.
  Without the reconfigure you get "No rule to make target".
  - A new source added to an existing target's list is picked up by the next build.
  - Only `src/{rig,text,content,shader,glsl,opengl,render_opengl,window}` and `tests/{rig,text}` are
    globbed. Everything else, including all of `examples/`, is listed file by file in `CMakeLists.txt`.
- **One file, several targets.**
  - `examples/support/spaceflight_scene.cpp` is compiled into `vng_spaceflight_scene_tests`,
    `vng_spaceflight_support` and `vng_scene_demo_support`.
  - `sun_assets.cpp` and `sun_options.cpp` are also compiled directly into their tests.
  - If you add a dependency to one of these files, add the link to every such target.
- **Fan-out.** `examples/support/earth_*` builds `vng_earth_assets`, which `vng_editor_project` links
  PUBLIC. Touching an Earth `.cpp` therefore relinks most editor, scene and test executables. Touching
  an Earth header also recompiles every file that includes it, including parts of the editor runtime,
  the editor project and the tunnel scene.
- **Warnings.** Every compiled vng target uses `-Wall -Wextra -Wpedantic -Wconversion`. Warnings are
  not errors, but don't add any. Only recompiled files report warnings, so check your own build output.
  A fresh build dir recompiles everything and prints many pre-existing warnings ([status.md](status.md#build-directories)),
  so filter for your own files:
  `cmake --build build -j 12 --target <t> 2>&1 | grep -E '<your_file_stem>.*(warning|error)'`.

**Syntax-check one `.cpp` without building.** This uses the exact flags from
`build/compile_commands.json`, writes nothing to `build/`, and takes about 1-3 s per file (1 s for
`scene_file.cpp`, 3 s for `app.cpp`). It is safe while someone else is building. It also prints
warnings. Headers are not in the database, so check a `.cpp` that includes them.

```sh
F=examples/editor/scene_file.cpp
python3 - "$F" <<'EOF'
import json, os, shlex, subprocess, sys
path = os.path.abspath(sys.argv[1])
entries = [e for e in json.load(open("build/compile_commands.json")) if e["file"] == path]
if not entries: sys.exit(f"{path} is not in build/compile_commands.json (headers are not; pick a .cpp that includes it)")
args = shlex.split(entries[0]["command"])
i = args.index("-o"); del args[i:i + 2]
args = [a for a in args if a != "-c"] + ["-fsyntax-only"]
sys.exit(subprocess.run(args, cwd=entries[0]["directory"]).returncode)
EOF
```

## 3. A build dir is tied to its source tree

- **Absolute paths are baked in.**
  - Tests and scene demos read the `examples/assets` of the tree that configured them.
  - The editor's "Reload C++" button runs `cmake --build <its build dir> --target vng_editor_worker -j 2`
    in its own source tree.
  - A binary from another checkout's `build/` therefore reads, and builds, *that* checkout.
    Always run `./build/...` from your own worktree.
- **Assets copied at configure time.** These are copied into `build/examples/assets/`:
  `spaceship.vmesh`, `spaceflight.vscene`, `solar_flyby.vscene`, `quiet_sun.veffect`,
  `colored_triangle.vmesh`, `colored_cube.vmesh`, `custom_scene.vscene`, and the fonts.
  - After you edit one, run any `cmake --build` (it reconfigures) before you run a demo that uses it.
  - The scene demos and most tests read `examples/assets` directly.
- **Shared files in a build dir.**
  - `build/Testing/Temporary/LastTest.log` and `LastTestsFailed.log` belong to whoever ran `ctest`
    last. Take your results from your own output (`--output-on-failure`, or `--output-log <file>`).
  - Every e2e run through `ctest` keeps a `build/editor-e2e/run-XXXXXX/` directory with screenshots,
    even when it passes, so these grow to gigabytes. When you run the e2e binary by hand, pass
    `--artifacts` ([§4](#editor-end-to-end-e2e)).

## 4. Run tests

If your harness resets the working directory between commands, start each one with
`cd <worktree root> &&`.

### CTest

```sh
ctest --test-dir build -N                                      # list the tests
ctest --test-dir build -N --print-labels
ctest --test-dir build -R '^vng_timeline_tests$' --output-on-failure
ctest --test-dir build -L compile-fail -j 12                   # no build needed, about 2-3 s
ctest --test-dir build -LE 'opengl|window|compile-fail' -j 12  # CPU lane, works headless, about 70 s
```

Anchor your `-R` regexes. For example, `-R 'vng_editor_'` matches 11 tests, including all five e2e runs
and the 90 s runtime test.

| Label | What it selects | Gap |
|---|---|---|
| `compile-fail` | Every compile-fail test | None |
| `opengl` | Every GPU test, including e2e | None |
| `e2e` | The five editor walkthroughs (RUN_SERIAL, TIMEOUT 300) | None |
| `window` | `vng_window_glfw_tests`, `vng_x11_scroll_tests`, `vng_glfw_opengl_tests` | All three show a window; the third only briefly ([below](#gpu-tests-skips-and-the-owners-desktop)) |
| `editor` | GPU editor tests, plus the Earth and asteroid render tests | **Misses** `vng_editor_tests`, `vng_editor_ui_tests`, `vng_editor_preview_tests` |
| `examples` | Earth, asteroid, sun and spaceflight render tests | Misses every scene CPU test and `vng_tunnel_render_tests` |
| `ui`, `text`, `rig`, `resources` | `ui`: `vng_ui_tests`, `vng_ui_opengl_tests`. The other three hold only their GPU test (`vng_text_opengl_tests`, `vng_rig_opengl_tests`, `vng_resource_owner_tests`) | **Miss** `vng_text_tests`, `vng_rig_tests`, `vng_resources_tests` |
| (no label) | Most CPU tests | Select them with `-R` |

Don't rely on `-L editor` or `-L examples` for coverage. Select tests with `-R` instead.

### Catch2 filters

Most test binaries are Catch2 v3. Run them directly to filter:

```sh
./build/vng_editor_tests --list-tags
./build/vng_editor_tests "[camera]" --list-tests
./build/vng_editor_tests "[timeline]"
./build/vng_editor_tests -# "[#picking_acceleration_tests]"   # every case in one source file
./build/vng_timeline_tests "*interpolation*"                   # test-name wildcard
./build/vng_editor_tests "[camera]" --durations yes
```

- `"[a][b]"` means AND, `"[a],[b]"` means OR, and `"~[a]"` means NOT.
- A filter that matches nothing prints "No tests ran" and exits 2. A bare `"#file"` without `-#`
  matches nothing.
- Two test binaries are not Catch2: `vng_editor_worker_tests` (`--animation-only`
  runs its hidden animation-patch check; no Catch2 filters) and
  `vng_editor_e2e_tests` (only the flags below). `vng_editor_preview_tests` has its
  own `main` too, but it hands the arguments to Catch2, so it filters normally.

### Editor end-to-end (e2e)

```sh
./build/vng_editor_e2e_tests --multi --artifacts "$S/e2e"    # $S: your output folder (collaboration.md); ~9 s, hidden
```

- The walkthroughs: no flag runs the main one; the others are `--fleet`, `--multi`, `--popout` and
  `--earth`.
- The binary prints "E2E artifacts: <dir>". That directory holds `index.html`, `report.json`, screenshots,
  `*.ui.txt` and `worker.log`.
- Through CTest, these runs are named `vng_editor_{fleet,multiselect,popout,earth}_e2e_tests`, and the
  main one is `vng_editor_e2e_tests`. They are all built by the single target `vng_editor_e2e_tests`.
- `--fleet` and `--earth` build their fixtures from the asteroid and Earth generators; they don't read the
  committed `.vscene` files.
- The editor window and the popped-out viewport stay hidden under automation (`app.cpp` passes
  `!options.automation` as the visibility of both). Independent Play, however, shows the preview
  worker's own window (`editor_worker.cpp`, `set_play`), so `--popout` briefly shows one on the desktop.
- More detail: [editor.md](editor.md) and [docs/editor.md "Validation"](../editor.md#validation).

### GPU tests, skips and the owner's desktop

- **Skips.** GPU and window tests exit 77 when there is no GL 4.6 context or display, and CTest reports
  them as "Skipped". It still prints "100% tests passed", and the skipped tests appear only under
  "The following tests did not run". Always read that list.
- **The display.** If `DISPLAY` is empty in your shell, every GPU test skips.
  - The owner's desktop is X11 display `:1` (`ls /tmp/.X11-unix` shows `X1`). Run `export DISPLAY=:1`.
  - There is no `xvfb-run`, so GPU work runs on the real desktop, in hidden windows.
- **Quick GPU sanity check.** This runs in under 1 s, opens no visible window, and must exit 0
  (77 means skipped): `./build/vng_glfw_opengl_tests "~[modes]"`. Leave out `[modes]`: that case
  switches a hidden window to fullscreen, and GLFW maps it to do so.
- **What shows on the owner's desktop.**
  - `vng_window_glfw_tests` toggles a window visible.
  - `vng_x11_scroll_tests` starts a 640x480 Xephyr window.
  - `vng_glfw_opengl_tests` briefly shows a fullscreen window (its `[modes]` case).
  - `vng_editor_worker_tests` and the `--popout` e2e run briefly show the worker's window during
    Independent Play.
  - Every graphical `vng_*_demo` opens a window, including runs with `--screenshot` or `--analyze`.
    Only `vng_document_demo` is CPU-only and opens none.
  - `vng_editor_demo` opens a window, even with `--once`.
  - Other GPU tests, the other e2e runs and `vng_scene_frames` stay hidden.
  - Skip the two window-only tests (`vng_window_glfw_tests`, `vng_x11_scroll_tests`) unless you changed
    window code.

### Long commands and tool timeouts

Agent harnesses often kill a command after about 2 min; Claude Code's Bash tool defaults to 120 s
and allows at most 600 s. For anything marked **(long)** in [§6](#6-durations-debug), either pass
an explicit timeout or run it in the background and poll its log.

## 5. Which tests for which change

Build the targets first, then run them:

```sh
T='vng_shader_tests|vng_glsl_tests'                  # names from the table
cmake --build build -j 12 --target ${T//|/ }
ctest --test-dir build --output-on-failure -R "^($T)\$"
```

In the table, `ctest …` is short for `ctest --test-dir build …`, and a bare test name or flag
means that binary or ctest label. The node tests and compile-fail tests need no build. The four
e2e variants are built by the `vng_editor_e2e_tests` target. Before you hand off, run the whole suite
([§8](#8-before-you-hand-off)). For a coarser map, see
[docs/codebase.md](../codebase.md#choose-the-narrowest-implementation-boundary).

For a header the table doesn't name, list the targets that compiled it in the last build (from the
depfiles), then run the test executables among them:

```sh
grep -rl --include='*.o.d' 'editor/camera_walk_logic.hpp' build/CMakeFiles \
  | sed 's#build/CMakeFiles/##; s#\.dir/.*##' | sort | uniq -c | sort -rn
```

| You changed | Run |
|---|---|
| Anything (always) | The node tests (no build). The guide quotes real source, so editing quoted code fails with "Outdated … excerpt: <topic> / <file>". Renaming an editor role class that `content.test.cjs` lists (e.g. `CameraPointerLogic`, `EditingWorkspaceUI`) fails until the list and [docs/editor_components.md](../editor_components.md) match ("Document the role of <Name>"). Fix either as described in [docs-maintenance.md](docs-maintenance.md). |
| `target_link_libraries` in `CMakeLists.txt`, or a new `include/vng/<m>` or `src/<m>` | The node tests, plus a reconfigure. The guide asserts that the dependencies it documents equal the CMake links. The layering test needs a `moduleTargets` entry for the new module ([engine.md](engine.md)). |
| `include/vng/core`, `include/vng/input` (header-only, used everywhere) | Full suite ([§8](#8-before-you-hand-off)) |
| `gfx` (records, layouts, meshes, codecs) | `vng_gfx_tests`, `vng_content_tests`, `-L compile-fail`, `vng_opengl_tests` |
| `shader`, or a new lowering case in `src/glsl/emitter.cpp` (e.g. a new opcode) | `vng_shader_tests`, `vng_glsl_tests`, `-L compile-fail`, `vng_render_opengl_tests`, `vng_opengl_tests` |
| `render`, `analysis` | `vng_render_tests`, `vng_analysis_tests`, `-L compile-fail`, then `ctest -L opengl -E e2e` **(long)** |
| `content` (documents, `.vmesh`, PNG) | `vng_content_tests`, `vng_document_tests`, `vng_spaceship_model_tests`, `-L compile-fail`, `vng_editor_tests` |
| `timeline` | `vng_timeline_tests`, `vng_editor_tests "[timeline]"`, `vng_tunnel_scene_tests` (the largest key counts) |
| `spatial` | `vng_editor_tests -# "[#picking_acceleration_tests]"`, then all of `vng_editor_tests` |
| `resources`, `providers`, `resources_opengl` | `vng_resources_tests`, `vng_resource_owner_tests`; for `providers`, also `ctest -L opengl -E e2e` **(long)** |
| `rig`, `rig_opengl` | `vng_rig_tests`, `vng_rig_opengl_tests` |
| `text`, `ui`, `text_opengl`, `ui_opengl` | `vng_text_tests`, `vng_ui_tests`, `vng_text_opengl_tests`, `vng_ui_opengl_tests`, `vng_editor_ui_tests`, `vng_editor_overlay_tests` |
| `opengl`, `render_opengl`, `bloom_opengl`, or a `glsl` change that alters existing GLSL output | `ctest -L opengl -E e2e` **(long)**; then `-L e2e` **(long)** if the editor draws differently. The set includes the three tests that flash a window ([§8](#8-before-you-hand-off)) |
| `window`, `glfw_opengl`, `cmake/glfw/*` | `vng_window_glfw_tests`, `vng_glfw_opengl_tests`, `vng_scroll_input_tests`, `vng_x11_scroll_tests` (all but `vng_scroll_input_tests` show a window) |
| `src/editor/*` (inspector, mesh, topology, patch) | `vng_editor_tests`, `vng_editor_ui_tests` |
| `src/editor/preview.cpp`, `include/vng/editor/preview.hpp`, `examples/editor_worker.cpp`, worker packets | `vng_editor_preview_tests`, `vng_editor_worker_tests`, then e2e `--popout` |
| `vng_editor_project` sources (`project`, `editing_session`, `animation`, `scene_file`, `settings`, `*_edits`, `preview_delivery_logic`, `camera_pointer_logic`, shortcut headers) | `vng_editor_tests`, `vng_editor_ui_tests`. If the document format, validation or animation changed, add `ctest -R '_scene_tests$'`, because the generators build on it. |
| `vng_editor_ui` sources (panels, dialogs, tools, `workspace_ui*`, `*_ui`, components) and the header-only `*_logic`, `*_gizmo` and `viewport_*` files they include | `vng_editor_ui_tests`, then e2e `--multi` and the main run (hidden). Add `--popout` only when you changed pop-out or Play code, and ask first: it briefly shows the worker's Play window. `-L e2e` **(long)** includes it |
| Editor runtime (`runtime.cpp`, `blueprint_mesh_renderer.cpp`, `mesh_programs.cpp`, `scene_annotations.cpp`, `mesh_shading.hpp`) | `vng_editor_runtime_tests` **(long)**, `vng_editor_overlay_tests`, `vng_tunnel_render_tests`, `vng_earth_render_tests`, `vng_asteroid_render_tests`. Rebuild `vng_scene_frames` before you review renders. |
| `examples/editor/app.cpp`, `mesh_overlay.cpp`, `viewport_window.cpp`, `preview_logic.hpp` (only `app.cpp` includes it) | `vng_editor_overlay_tests`, `-L e2e` **(long)** |
| Tunnel and departure (`examples/scenes/tunnel*`, `departure_voyage.cpp`, `key_batch.*`, `examples/support/space_assets.cpp`) | `vng_tunnel_scene_tests`, `vng_tunnel_render_tests`. No test reads the committed `tunnel_departure.vscene`, so regenerate it deliberately ([scenes-and-cinematics.md](scenes-and-cinematics.md)). |
| `examples/assets/earth_future.vmesh` (any regeneration or hand edit) | `vng_tunnel_scene_tests` and `vng_tunnel_render_tests`: both author the departure from it, and the scene test's flight-obstruction and camera line-of-sight checks are the ones a new part can break ([assets.md](assets.md#recipe-4-change-earths-infrastructure-by-hand-and-carry-it-into-the-cinematic)). `vng_earth_assets_tests` never reads `earth_future.*`; `vng_earth_render_tests` reads only `earth_future.vscene` |
| Earth (`examples/support/earth_*`, `examples/scenes/earth_*`) | `vng_earth_assets_tests` (about 70 s; filter by tag while iterating), `vng_earth_render_tests`, `vng_editor_earth_e2e_tests` |
| Fleet, asteroid and solar-system scenes, `asteroid_assets.cpp` | `vng_fleet_scene_tests`, `vng_asteroid_scene_tests`, `vng_solar_system_scene_tests`, `vng_asteroid_render_tests`, `vng_editor_fleet_e2e_tests` (it authors the *asteroid* scene), `vng_tunnel_scene_tests` (the voyage reuses the belt's rocks) |
| Sun (`examples/support/sun_*`) | `vng_sun_assets_tests`, `vng_sun_options_tests`, `vng_sun_render_tests`, and `vng_editor_runtime_tests` **(long)**, because the runtime links the sun |
| Spaceflight (`examples/support/spaceflight_*`, `spaceship_renderer`, `space_background`) | `vng_spaceflight_scene_tests`, `vng_spaceflight_render_tests`, `vng_spaceship_model_tests` |
| `examples/tools/*` | No tests. Build the tool by name and run it with a scratch output path ([assets.md](assets.md)). |
| Docs only | The node tests if you touched `docs/explore/`, `docs/codebase.md` or `docs/editor_components.md`. No C++ build is needed. |

## 6. Durations (Debug)

Measured on the owner's machine in 2026-09. CTest prints each test's time, so re-run a test to re-measure it.
**(long)** means over 60 s: set a tool timeout.

| What | Time |
|---|---|
| Both node tests | 0.2 s |
| `ctest -L compile-fail -j 12` | 2-3 s |
| Engine CPU binaries (timeline, gfx, render, shader, glsl, content, document, resources, rig, text, ui, analysis) | under 2 s each |
| `vng_glfw_opengl_tests`, `vng_editor_preview_tests` | under 2 s |
| `vng_editor_tests` / `vng_editor_ui_tests` | 6 s / 16 s |
| `vng_editor_overlay_tests` / `vng_editor_worker_tests` (TIMEOUT 360) | 6 s / 24 s |
| `vng_editor_runtime_tests` (no CTest timeout) | about 90 s **(long)** |
| `vng_tunnel_scene_tests` / `vng_tunnel_render_tests` | 12 s / 11 s |
| `vng_asteroid_scene_tests` / `vng_asteroid_render_tests` | 15 s / 17 s |
| `vng_fleet_scene_tests`, `vng_solar_system_scene_tests`, `vng_sun_render_tests`, `vng_spaceflight_render_tests` | 3-6 s |
| `vng_earth_assets_tests` (`"[assets]"` about 2 s, `"[redesign]"` about 5 s) | about 70 s **(long)** |
| `vng_earth_render_tests` | about 35 s |
| e2e: main / earth / fleet / multi / popout | 47 / 38 / 34 / 9 / 8 s |
| `ctest -L e2e` (serial) | about 2.3 min **(long)** |
| `ctest -L opengl -E e2e -j 12` | about 1.5 min, dominated by the runtime tests (estimate) **(long)** |
| CPU lane `-LE 'opengl\|window\|compile-fail' -j 12` | about 70 s, dominated by `vng_earth_assets_tests` (estimate) **(long)** |
| Full `ctest -j 12` | about 4 min **(long)** |
| Configure / full build from scratch | about 2 s / several minutes **(long)** |

## 7. Add tests and targets

- **A test case.** Add `TEST_CASE("…", "[area]")` to one of the executable's sources and rebuild. To
  find which executable owns a file, run `grep -n '<file>.cpp' CMakeLists.txt`.
- **A test source file.** Add it to the executable's list in `CMakeLists.txt` (only `tests/rig` and
  `tests/text` are globbed).
- **A test executable.** Copy the `vng_timeline_tests` block: `add_executable`,
  `target_link_libraries(… Catch2::Catch2WithMain)`, `vng_target_defaults`, `add_test`.
  - Pass asset paths as absolute compile definitions (`VNG_…="${CMAKE_CURRENT_SOURCE_DIR}/examples/assets/…"`).
  - Add a label if a label lane should include the test.
  - Then run `cmake -S . -B build`.
- **A GPU test executable.** Do everything in the previous item, and also:
  - Put it inside the matching `if(...)` block.
  - Add `set_tests_properties(<name> PROPERTIES SKIP_RETURN_CODE 77 LABELS "opengl")`.
  - Create the context with `vng::test::create_hidden_opengl_window` (`tests/support/glfw_opengl.hpp`),
    and call `std::exit(77)` if that fails. `tests/opengl/opengl_tests.cpp` shows the pattern.
- **A compile-fail test.**
  1. Write `tests/compile_fail/<name>.cpp`. Include only `<vng/...>` and standard-library headers (the DSL
     cases start with `#include <vng/shader/dsl/dsl.hpp>`); the runner adds no include path except `include/`.
  2. Register it. For a `literal_<case>.cpp` whose expected diagnostic is "constant expression", add `<case>` to
     the `foreach(operand_case ...)` list in `CMakeLists.txt`; the loop names it `vng_compile_fail_literal_<case>`.
     Any other case gets its own call next to the others:
     `vng_add_compile_failure_test(vng_compile_fail_<name> tests/compile_fail/<name>.cpp "expected diagnostic substring")`.
  3. Reconfigure.
  - The runner is `tests/compile_fail/run.cmake`. It runs the configured compiler with
    `-std=c++23 -I include -fsyntax-only` and no warning flags.
  - The test passes only if compilation fails *and* the output contains the substring.
  - The substrings are only known to match g++. With the installed clang++ 18, 29 of the 42 fail
    before they reach the expected diagnostic, because libstdc++ 14's `std::expected` is unavailable
    under that clang.
- **An engine module.** See [engine.md](engine.md).
- **A scene, generator or tool target.** See [scenes-and-cinematics.md](scenes-and-cinematics.md).
  Keep tools EXCLUDE_FROM_ALL, as the existing ones are.

## 8. Before you hand off

1. **Take a baseline before you edit.** Run the tests you plan to use on the untouched tree, so you
   can tell your failures from failures that already existed ([status.md](status.md) lists the known ones).
2. **Rebuild everything, plus every tool you touched or used:**
   `cmake --build build -j 12 && cmake --build build -j 12 --target <tools>`. A build made by naming targets
   lacks most test executables, and ctest reports each missing one as a "Not Run" failure ("Could not find
   executable"). The full build takes several minutes and ~4.3 GB of disk (§1).
3. **Run the full suite without the two window-only tests.** It takes about 4 min **(long)**. Three tests in it
   flash a window on the owner's desktop: `vng_glfw_opengl_tests` (its fullscreen `[modes]` case), and
   Independent Play in `vng_editor_worker_tests` and `vng_editor_popout_e2e_tests`. Like any visible window, they
   need the owner's OK in chat ([collaboration.md §3](collaboration.md#3-approvals)); an OK covers what the owner
   said, for example "every run this session". Without it, exclude them, run the hidden GL check instead, and name
   the exclusions in your `Verified:` line:
   ```sh
   ctest --test-dir build -j 12 --output-on-failure -E '^vng_(window_glfw|x11_scroll)_tests$'   # with the owner's OK (89 tests)
   ctest --test-dir build -j 12 --output-on-failure \
     -E '^vng_(window_glfw|x11_scroll|glfw_opengl|editor_worker|editor_popout_e2e)_tests$'      # no windows (86 tests)
   ./build/vng_glfw_opengl_tests "~[modes]"                                                    # hidden; must exit 0
   ```
4. **Report exactly what ran.** Give counts, every failure, what was skipped and what you excluded.
   The format is in [collaboration.md](collaboration.md#the-verified-line). For example:
   `Verified: rebuilt all + vng_scene_frames; ctest -j 12 -E 'window_glfw|x11_scroll': 89/89 passed, 0 skipped (DISPLAY=:1); node tests pass.`
