# Editor guide for agents

Read this when you change anything in `examples/editor/`, `examples/editor.cpp`, `examples/editor_worker.cpp`,
`include/vng/editor/`, `src/editor/` or `tests/editor/`, or when you need to run the editor.
[AGENTS.md](../../AGENTS.md) comes first: its safety, build and test rules apply here too.

**TL;DR**
- There are two processes. The UI (`vng_editor_demo`) owns the authoritative document: `EditingWorkspaceUI` holds the
  only mutable `EditingSession`. The worker (`vng_editor_worker`) renders its own copy. They exchange packets over a
  socket and share memory only for the latest preview image.
- Every authored edit goes through `EditingSession`. Property edits need a selected keyframe at the paused playhead.
- Components follow parent-owned coordination: children observe a `const EditingSession&` and return proposals, and
  their parent executes them. Viewport input is delivered one occurrence at a time, in order. Read section 6 before you
  add a panel, tool or gizmo.
- Prefer hidden runs: CPU tests first, then the e2e walkthroughs with `--artifacts`. Run `vng_editor_demo` only with a
  scratch `XDG_CONFIG_HOME` and a *copy* of the scene (section 3).

## 1. Mental model

| Piece | Where | Owns |
|---|---|---|
| UI process | `examples/editor.cpp` → `editor_example::run` in `examples/editor/app.cpp` (host and composition root, ~2.4k lines) | `EditingWorkspaceUI` (session, selection, viewport tools, lists, timeline), `PreviewLogic`, windows, toolbars, host panels and dialogs |
| Preview worker | `examples/editor_worker.cpp`, target `vng_editor_worker`, started from a copy under `/tmp/vng-preview-XXXXXX/` | its own `State` copy, GL context, `Runtime`, native inspector callbacks, the "Play (independent)" window |
| Transport | `include/vng/editor/preview.hpp`, `src/editor/preview.cpp` (`PreviewSession`), owned in the UI by `PreviewLogic` (`examples/editor/preview_logic.hpp`) | Unix seqpacket socket + shared-memory latest-image slot |

- **Data.** `State = {Document document; ViewportState viewport;}` in `examples/editor/project.hpp`. Read that file first.
  - `Document` is authored content, saved to `.vscene`.
  - `ViewportState` is private: mode, selection, playhead, editor camera, view toggles. A saved file (`encode_scene`)
    keeps only mode, inspected mesh, selected object and vertex, `weld`, `paused` and `time`. The editor camera,
    `pilot_camera` and the view toggles go only to the worker (`encode`; `include_editor_view` in `project.cpp`),
    not to disk. The reader still accepts them from older files.
- **Content** is blueprints (shared geometry or effect defaults) plus instances. An instance has an id, a blueprint, a
  name, a transform and its own `settings` variant: `MeshSettings | SunSettings | RegionSettings | CameraSettings | AnimationSettings`.
- **Animation** is a neutral `vng::timeline::Timeline` addressed by `(instance id, property name)`. `animation.*` maps
  it onto instances. Evaluation never mutates the document.
  Scene-owned animation instances (`scene_animation.*`) additionally override explicitly claimed properties
  during their intervals. Their typed children evaluate first; the parent combines results. Enabled roots may
  share targets only when their property/time scopes do not overlap. `AnimationFrame` shares sampled results;
  parameter edits use `DocumentChanges::animations` and root-only document patches. See
  [animation forests](../editor_components.md#scene-animation-forests).
- **Edits flow** as follows. `EditingSession` publishes an `EditNotice` (revision + `DocumentChanges`) through
  `take_changes()`. The host passes the changes to `PreviewLogic`, whose `PreviewDeliveryLogic`
  (`preview_delivery_logic.*`) turns them into narrow patches for the worker. Structural edits (import, create/delete
  instance, load, reconnect) send a whole-scene snapshot instead. If a patch is bad, the worker replies `resync\n…` and
  gets a snapshot.
- **Which camera shows.**
  - The viewport renders the editor's private camera (`ViewportState::editor_camera`).
  - "Play (independent)" renders the scene's active camera instance, via `render_camera(state, t,
    maximum_viewing_distance)`. Its far plane is the editor preference "Maximum viewing distance" (default 10,000;
    the worker's embedded preview uses it too), *not* `environment.view_distance`
    as in demos and `vng_scene_frames` ([rendering-and-review.md](rendering-and-review.md)).
  - Independent Play refuses to start without a camera instance, and the default project has none.
  - `ViewportState::pilot_camera` (the old "edit animation camera" mode) still exists, and `--help` still mentions it,
    but `app.cpp` never sets it.

Orientation reads: [docs/codebase.md](../codebase.md) sections "Editor ownership" and "Follow one interaction",
[docs/editor_components.md](../editor_components.md) (ownership tree, proposals, ordered input, gizmo bindings),
[docs/editor_boundaries.md](../editor_boundaries.md), [docs/editing_session.md](../editing_session.md).

## 2. Layers, stability and tests

| Layer | Main files (`examples/editor/` unless noted) | Target | Stability | First tests |
|---|---|---|---|---|
| Data + validation | `project.*`, `animation.*`, `keyframes.*`, `scene_file.*`, `document_patch.*`, `document_changes.hpp`, `camera_instances.cpp`, `settings.*` | `vng_editor_project` | stable | `vng_editor_tests` |
| Authoring boundary | `editing_session.*`, `session_operations.cpp`, `instance_movement.hpp` (typed move edit) | `vng_editor_project` | stable; new operations get added | `vng_editor_tests "[session]"` |
| Transport | `preview_delivery_logic.*`, `viewport_session.hpp`, `preview_logic.hpp`, `preview_mailbox.hpp`, `include/vng/editor/preview.hpp`, `src/editor/preview.cpp`, `examples/editor_worker.cpp` | `vng_editor_project`, `vng_editor_preview`, worker (`preview_logic.hpp` is header-only) | stable | `vng_editor_tests` (`update_tests.cpp`), `vng_editor_preview_tests`, `vng_editor_worker_tests` |
| Rendering | `runtime.cpp`, `mesh_shading.hpp`, `blueprint_mesh_renderer.cpp`, `mesh_programs.cpp`, `scene_annotations.cpp` | `vng_editor_runtime` | stable | `vng_editor_runtime_tests`, scene `*_render_tests` |
| UI components | `workspace_ui.hpp` + `workspace_ui_*.cpp`, `workspace_{selection,situation}.hpp`, `viewport_{tools_ui,input,context,picking}.hpp`, `mesh_{editing_ui,tools_ui,menu}.*`, `timeline_editing_ui.*`, `scene_lists.*`, `move_gizmo.hpp`, `camera_gizmo.*`, `camera_walk_logic.hpp`, `*_tool.*`, `*_panel.*`, `*_dialog.*`, `component_dispatch.hpp` | `vng_editor_ui` (many header-only) | rules stable (section 6); names and context fields still change | `vng_editor_ui_tests`, e2e |
| Composition root | `app.cpp`, `mesh_overlay.cpp`, `viewport_window.cpp` | `vng_editor_app` (static library) | churns with the UI layer | `vng_editor_overlay_tests` (`mesh_overlay.cpp`), e2e |

`camera_pointer_logic.cpp` is in `vng_editor_project`, not `vng_editor_ui`, because the worker also uses it.
The runnable program is `vng_editor_demo`, which links `vng_editor_app`. Building it (or `vng_editor_e2e_tests`) also
builds `vng_editor_worker`.

## 3. Run the editor safely

Pick the first rung that answers your question.

| Rung | What | Window |
|---|---|---|
| 1 | CPU tests, `vng_scene_probe SCENE --list \| --size \| ID TIME...` | none |
| 2 | `vng_editor_e2e_tests --artifacts DIR`, `vng_editor_worker_tests`, `vng_scene_frames` | hidden, except the worker's Play window (below) |
| 3 | `vng_editor_demo` | **visible on the owner's desktop**; only when the task needs it |

Rung 2, a real UI walkthrough with screenshots (hidden window, private preferences and fixtures). `$S` is your output
folder ([collaboration.md](collaboration.md#create-your-worktree)):
```sh
S="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")/.claude/worktrees/<task>.out"
./build/vng_editor_e2e_tests --multi --artifacts "$S/e2e"    # ~9 s, hidden; prints "E2E artifacts: <dir>"
```
- Each run creates `$S/e2e/run-XXXXXX/` holding `index.html`, `report.json`, `*.png`, `*.ui.txt`, `worker.log` and
  `settings.conf`. Open the PNGs directly.
- Without `--artifacts`, runs pile up under `build/editor-e2e/` forever.
- Under automation the UI window and the pop-out viewport are hidden. The worker's Play window is not: its
  `set_play` calls `window_.show()`, and the worker gets no automation flag. `--popout` presses "Play (independent)"
  and `vng_editor_worker_tests` sends `play\n1`, so both should briefly show a window on the desktop (read from the
  code; not observed). Ask the owner before you run either
  ([build-and-test.md §8](build-and-test.md#8-before-you-hand-off)).

Rung 3, interactive or one-shot capture:
```sh
S="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")/.claude/worktrees/<task>.out"
mkdir -p "$S/config"
cp examples/assets/editor_timeline.vscene "$S/scene.vscene"      # copy, never symlink (symlinks are rejected)
XDG_CONFIG_HOME="$S/config" ./build/vng_editor_demo --windowed --scene "$S/scene.vscene"
# One frame, then exit (window still visible; fails after 120 s; the PNG must not exist yet):
XDG_CONFIG_HOME="$S/config" ./build/vng_editor_demo --windowed --once --screenshot "$S/editor.png" --scene "$S/scene.vscene"
```
- `editor_timeline.vscene` is tiny and has no camera. For Independent Play, copy a scene that has a camera instance:
  check with `./build/vng_scene_probe SCENE --list | grep camera`.
- Only editor projects open (`mesh_data` plus `editor_project = 1` to `4`). `spaceflight`, `solar_flyby` and
  `custom_scene` use other schemas and fail with `Required property is missing` before a window opens.

What a visible run can damage, and the guard for each:

| Action | Effect | Guard |
|---|---|---|
| Changing Settings, camera preferences, UI scale or the scroll toggle | rewrites `$XDG_CONFIG_HOME/vng/editor.settings`, else `~/.config/vng/editor.settings` (the owner's real file) | `XDG_CONFIG_HOME=<absolute path>`. A relative value is ignored and falls back to `$HOME` |
| Save / Ctrl+S | atomically overwrites the opened scene, rewriting it as `editor_project = 4` and baking mesh placements into geometry | open a copy |
| Save As | creates a file (mode 0600) and confirms before replacing | save into your scratch dir |
| Reload C++ | runs `cmake --build <build dir baked in at configure> --target vng_editor_worker -j 2` in the baked source dir | only from a binary built in your own worktree |
| Play (independent) | opens a second window (the worker's) | none needed |

The editor refuses automation without a private preferences path (`options.preferences`, checked at the top of
`run`), and the e2e driver always passes one, so tests never touch the owner's settings.

## 4. Build and test ladder

```sh
cmake --build build --target vng_editor_tests vng_editor_ui_tests -j 12
./build/vng_editor_tests "[session]"          # Catch2 filter; --list-tags shows ~85 tags
```

| Step | Command | Debug time | Run when |
|---|---|---|---|
| 1 | `./build/vng_editor_tests` | ~6 s | always (model, session, files, shortcuts, settings, delivery) |
| 2 | `./build/vng_editor_ui_tests` | ~16 s | panels, tools, workspace, gizmos, input (`"[workspace]"`, `"[parent-coordination]"`, `"[movement-binding]"`, `"[workspace-picking]"`, `"[input-routing]"`, `"[dispatch]"`) |
| 3 | `./build/vng_editor_preview_tests` | ~2 s | `src/editor/preview.cpp` |
| 4 | `./build/vng_editor_worker_tests` | ~24 s | protocol, worker, runtime. Not Catch2. Starts Independent Play (section 3); `--animation-only` runs only the hidden animation-patch check |
| 5 | `./build/vng_editor_runtime_tests` | **~90 s** | `runtime.cpp`, `mesh_shading.hpp`, renderers. Set a tool timeout |
| 6 | `./build/vng_editor_e2e_tests [--popout\|--multi] --artifacts "$S/e2e"` | ~8 s each; main run (no flag) ~47 s | any UI or `app.cpp` change. Not Catch2 |
| 7 | `./build/vng_editor_e2e_tests --fleet\|--earth --artifacts "$S/e2e"` | ~34 s and ~38 s | scene generators, big-scene UI paths |
| 8 | `node --test docs/explore/tests/content.test.cjs` | <1 s | you changed code quoted in `docs/explore/` (several `examples/editor/` headers, among them `project.hpp`; the command that lists them is in [docs-maintenance.md](docs-maintenance.md#3-quoted-code-drift-checked-excerpts)) |

- The times are from a Debug build.
- All five e2e runs together: `ctest --test-dir build -L e2e --output-on-failure` (about 2.3 min, RUN_SERIAL). This
  needs a tool timeout above 2 min.
- `ctest -R 'vng_editor_'` runs 11 tests, including all e2e and the runtime test. Budget several minutes.
- `ctest -L editor` skips the three CPU editor suites, because they have no label.
- The `--fleet` and `--earth` walkthroughs author their fixtures from `example::asteroids::author_scene` and
  `example::earth::author_scene`. A generator change can therefore break editor e2e, even though the committed
  `.vscene` files are untouched.
- Which tests to run for non-editor changes: [build-and-test.md](build-and-test.md).

## 5. Recipes

### Add an animatable property or instance field
Pick a sibling property and find every site that uses it: `grep -rn '"focus"' examples/editor` (camera) or
`'"brightness"'` (mesh). Then touch each layer:
1. **Struct.** `project.hpp` (`CameraSettings`, `MeshSettings`, …).
2. **File format.** In `project.cpp`, update `write_settings`, the matching `read_*` (use `reader.get_or(...)` so older
   files still load) and `valid_settings`. There is no version bump for an optional field; the writer always emits
   `editor_project = 4`.
3. **Animation.** In `animation.cpp`, add an `AnimationProperty` in `animation_properties()` with its min and max, and
   sample it in `evaluate_instance()`.
4. **Patch base value.** `document_patch.cpp` maps property names to fields
   (`if (key == "focus") return &settings.focus;`).
5. **Inspector.** In `effects.cpp`, add the control in `ProjectControls::describe_editor` and extend
   `apply_lens`/`apply_appearance`.
6. **Camera-specific.** For camera properties, also update `key_camera` (`animation.cpp`) and the property lists in
   `session_operations.cpp`.
7. **Rendering.** Use the value in `runtime.cpp`.
8. **Tests.** Add them to `tests/editor/{project,animation,camera_instance,effects}_tests.cpp`, including an
   `encode`/`decode` round trip.

Rules:
- Tracks naming an unknown `(object, property)` make the **whole file fail to load**. Never rename or remove a property
  without migrating saved scenes.
- Bool keys are forced to hold.

### Add an editor preference
1. Add the field with its default to `Settings` in `settings.hpp`. Give a numeric field a range check in
   `validate_settings`. A bool needs none: encode it as `0`/`1` and reject values above 1 in the decoder, as VSync and
   `scroll_moves_camera` do.
2. In `encode_settings`, bump the header to the next unused version and append the value at the end. Read the
   current number in `encode_settings`. Then check that no other checkout is bumping it too
   ([collaboration.md](collaboration.md#check-for-in-flight-edits-to-your-files); [status.md](status.md) lists known
   ones). Two branches that both claim version N write files that the other rejects or mis-reads, so ask the owner
   which lands first.
3. In `decode_settings`, add `const bool versionN = text.starts_with("vng-editor-settings N\n");` and chain it into
   the previous flag, as `version7 = version8 || …` does, so new files still read every older field. Read the new
   field only for the new version, with an `if (versionN) { ... }` placed after the last read inside the newest
   version block (today `if (version8) { ... }`), because that block's `read` lambda is local to it. Older files keep
   the default.
4. **The whole file must be ≤ 256 bytes** (checked on load and on decode). Floats print with `max_digits10`.
   - The defaults encode to 107 B. The longest valid version-8 file is 203 B.
   - Its longest values are `debug_fps = 4294967295` (the only uncapped field; the other FPS caps stop at 240) and
     9-significant-digit floats below 0.01, such as `0.00123456791F`, for the orbit range, the three walk speeds and
     the three drag speeds (13 characters each; maxima print shorter: `1e6F` prints `1000000`). Add
     `maximum_viewing_distance = 1.23456791F`, `walk.fast_multiplier = 12.3456789F`, 16384 tracks, 65536 instances and
     UI scale 150.
   - No test covers this yet. Add one that encodes such values and checks `validate_settings`, `size() <= 256` and a
     decode round trip.
5. Add the UI in `settings_panel.hpp` (Settings dialog) or `camera_preferences.hpp` (camera menu).
   - **The Settings dialog is full.** Its rows take about 883 of the 900 px it gets (`place(settings_host, …)` in
     `app.cpp` caps it at `std::min(900.F, size.y)`; `Fixture::host` in `tests/editor/settings_panel_tests.cpp` is
     `.height(900)`). One more stacked 28 px row makes it scroll, and `labels_fit()` then fails in the 8 of the 12
     `"[settings]"` cases of `vng_editor_ui_tests` that call it. A checkbox fits in a row beside VSync, e.g.
     `auto toggles = panel_.row().height(28).padding(0).gap(8);` with the VSync checkbox at `.width(450)` and yours at
     `.width(200)`. For anything taller, ask the owner before you enlarge the dialog.
   - `SettingsPanel::poll` starts from `draft_` (a copy of the whole `Settings`), so read your control in `poll()`
     and write it in `set()`. `CameraPreferences::read` also keeps the fields it doesn't show.
6. Persisting needs no `app.cpp` change. `SettingsPanel::poll` returns the whole `Settings`; the
   `if (settings_was_open)` block in `app.cpp` saves it with `save_settings(preferences_path, …)`, copies it into
   `settings` and resends it to every worker generation (`send_settings`). Touch that block only if the UI process
   itself must act on the value, as it does for VSync and the two limits.
   - The worker decodes the same text from its `settings\n` packet (`examples/editor_worker.cpp`). A UI and a worker
     built from different sources reject each other's newer files, so rebuild both (building `vng_editor_demo` or
     `vng_editor_e2e_tests` does).
   - Tests: `tests/editor/settings_tests.cpp` (`"[settings]"`) and `tests/editor/settings_panel_tests.cpp`. The main
     e2e walkthrough (no flag, ~47 s, hidden) drives the Settings dialog; extend its "Settings saved privately" step
     in `tests/editor/e2e_tests.cpp`.

### Add a keyboard shortcut
1. Pick the owner by scope, and grep `examples/editor` for the `Key::` you want first, so you don't shadow a binding:
   - **Window-wide commands** (save/open, copy/paste/undo/redo, delete, viewport Tab, gizmo cycle): the pure helpers
     in `file_shortcuts.hpp`, `edit_shortcuts.hpp`, `delete_shortcut.hpp` and `viewport_shortcuts.hpp`. `run()` in
     `app.cpp` calls them with an `enabled` gate (dialogs, mode transitions, gestures) and acts on the result. Test in
     `tests/editor/{file_shortcut,edit_shortcut,viewport_shortcut}_tests.cpp` (`vng_editor_tests`).
   - **Mesh component keys** (1/2/3, A, H, …): `MeshEditingUI` in `mesh_editing_ui.cpp`. The workspace feeds it one
     key at a time and executes each proposed operation before the next (`route_viewport` in `workspace_ui.hpp`).
     Test in `tests/editor/workspace_tests.cpp`.
   - **Gizmo keys** (G/R/S/F/T, region 1/2/3): the `GizmoDescription` table in `gizmo_mode.hpp`. Arrow nudges go through
     `transform_keys.hpp` and `GizmoControls`. Test in `instance_transform_tests.cpp`, `component_transform_tests.cpp`
     or `region_ui_tests.cpp`.
2. Document it in [docs/editor.md](../editor.md). A main control also belongs in `--help` (`examples/editor.cpp`).

### Add a panel, tool or gizmo
- **A host panel or dialog that authors nothing** (preferences, file dialogs, timing): construct it in `run()` in
  `app.cpp` on its own host container, `place(...)` it with the other hosts, poll it after `Screen::update()` and act
  on the result. Test it headless in `vng_editor_ui_tests` with a `ui::Screen` fixture, as
  `tests/editor/settings_panel_tests.cpp` does.
- **Anything that changes the document** belongs under the owner whose branch it serves (section 6), never in `app.cpp`:
  1. Take `const EditingSession&` in the constructor if it must observe the document. Never take a mutable session or
     a mutation callback.
  2. Declare private `handle(const Situation&, const Context&)` overloads and `friend class <ImmediateOwner>;`. Put
     optional sections in the context struct for work that only some calls need.
  3. Return a proposal (a reply value, or `take_edit()` for queued or asynchronous results). The owner executes it on
     the session and acknowledges the result back down (`accept_edit`, `accept_operation`). The child updates its own
     state only after that acknowledgement.
  4. Give it a `debug_report()` and add it to its owner's report (Logs → Component diagnostics).
  5. Add a `.cpp` to `vng_editor_ui` ("Add a source file" below). Copy a test pattern: `workspace_tests.cpp`
     (menus, keyframes, narrow propagation), `blueprint_mesh_panel_tests.cpp` and `region_ui_tests.cpp`
     (proposal and acknowledgement).
- **A position gizmo** is `MoveGizmo<Binding>` (`move_gizmo.hpp`). The binding is a plain struct with `Target`, an
  owned `Edit`, and static `world_position(target)` and `move_to(target, destination)`; copy `instance_movement.hpp`.
  The owner calls `update`, `append` and `cancel`, and applies the `Edit`. Test in
  `tests/editor/movement_binding_tests.cpp`. Rotation, scale and custom tools keep their own classes
  (`instance_rotation_gizmo.hpp`, `scale_tool.hpp`, …).
- Viewport input for a new tool also needs a place in `ViewportToolsUI`'s priority (`ViewportTool`, `accepts`,
  `observe`) and in `EditingWorkspaceUI::interact_viewport` (`workspace_ui_viewport.cpp`). Test in
  `tests/editor/viewport_input_tests.cpp`.

### Add a source file
Editor sources are explicit lists in `CMakeLists.txt`; there are no globs. `cmake --build` re-runs CMake by itself
when you add a source to an existing target. A new target needs `cmake -S . -B build` first, or you get "No rule to
make target".

| Code | Target (`add_library`/`add_executable` name) |
|---|---|
| Model, validation, session, edits, settings, logic the worker shares (no UI, no GL) | `vng_editor_project` (and its `target_sources` lines) |
| Retained panels, dialogs, tools and workspace components drawn through `vng_ui`, no GL | `vng_editor_ui` |
| GL rendering used by the worker, demos and review tools | `vng_editor_runtime` |
| App wiring, mesh overlay, pop-out viewport window | `vng_editor_app` |
| Header-only component | no entry needed |
| CPU test, UI test | `vng_editor_tests`, `vng_editor_ui_tests` |

`examples/editor` must not include `../scenes`. Use relative `"../dir/file.hpp"` includes (see AGENTS.md section 7).

### Author an edit from code (workspace, host, tests)
```cpp
auto& view = editing.viewport();
view.paused = true; view.time = t;                // t must be a keyframe time (0 always is)
editing.select_keyframe(t);                        // loading never selects one, not even at t = 0
if (auto r = editing.begin_move(id, ids); !r) return std::unexpected(r.error());  // check every Result
if (auto r = editing.move(position); !r) { (void)editing.cancel(); return std::unexpected(r.error()); }
if (auto r = editing.commit(); !r) return std::unexpected(r.error());            // one undo entry per gesture
if (auto notice = editing.take_changes()) deliver(notice->changes);   // the host hands these to PreviewLogic
```
- `editing` is an `EditingSession` (the workspace's private one, or your own in a test). In `app.cpp` it is an alias
  for the `EditingWorkspaceUI`, which forwards the same calls. Inside a component, don't author: return a proposal
  (section 6).
- A gizmo proposal uses `EditingSession::apply(const InstanceMovement::Edit&)` instead of `move`. It rejects an edit
  for another object than the active one.
- Only one gesture or remote edit can be active at a time. If the playhead or `paused` changes mid-gesture, the next
  update fails ("Finish or cancel the edit before changing playback time").
- Generators do not use `EditingSession`. They build a `State` directly and key it with `key_property`/`key_camera`
  (`animation.hpp`) or, for thousands of samples, `example::KeyBatch` (`examples/scenes/key_batch.hpp`), which
  validates once at commit. See [scenes-and-cinematics.md](scenes-and-cinematics.md).

### Change `ViewportState` or the worker protocol
- **Viewport requests.** `encode_viewport_request`/`decode_viewport_request` in `viewport_session.hpp` are hand-rolled
  binary: a `VNGVIEW\4` tag, 76 bytes, and a flags word with 7 bits used. A new field needs a new tag, and the decoder
  must still accept the old versions. Also decide whether the saved `view = {…}` block in `project.cpp` should store it.
- **Worker packets.** The worker's packet loop in `examples/editor_worker.cpp` (the `starts_with("…\n")` branches) is
  the protocol list.
  - Add a branch, decode in the project layer, and answer bad input with `resync\n<message>`.
  - Never put commands or deltas on the latest-value lane (`PreviewSession::send_latest`, the `view` packets).
  - Test in `vng_editor_worker_tests`.
- **Schema changes.** Anything serialized in `State` needs the UI *and* the worker rebuilt. "Reload C++" rebuilds only
  the worker. It is meant for `effects.cpp`/`runtime.cpp` iteration, as `--help` says.

### Where other common changes live

| Change | Files | Tests |
|---|---|---|
| Widgets | `include/vng/ui/ui.hpp`, `src/ui/ui.cpp` ([docs/ui.md](../ui.md)) | `vng_ui_tests`, then the editor's `*_panel_tests.cpp`, `*_dialog_tests.cpp` |
| Lighting styles | `examples/editor/mesh_shading.hpp` (per-mesh `render/lighting` string) | `vng_editor_runtime_tests`, scene render tests. Rebuild the review tools |
| Sun look | `examples/support/sun_*.cpp` (`vng_sun_support`), [docs/sun.md](../sun.md) | `vng_sun_{assets,options,render}_tests` |
| New instance kind | Follow Region: `SceneInstance::settings` variant, `region_*.cpp`, `regions.cpp`, `animation_properties`, and `write_settings`/`read_region`/`valid_settings` in `project.cpp`. Find the rest with `grep -rln 'RegionSettings\|BlueprintId::region' examples/editor tests/editor`. This is a pointer list, not a traced recipe | [docs/regions.md](../regions.md) |

## 6. Component layer: parent-owned coordination

The parent-owned rework of this layer (ordered input, typed movement bindings) is on `main`. Its rules below are
stable. Component names, context fields and reply types still change with ongoing editor work, so check for newer
work before you edit it, and run the in-flight check in
[collaboration.md](collaboration.md#check-for-in-flight-edits-to-your-files):
```sh
git log --oneline -5 -- examples/editor/workspace_ui.hpp examples/editor/viewport_tools_ui.hpp docs/editor_components.md
```

Ownership (the full tree is in [docs/editor_components.md](../editor_components.md)):
```text
run() in app.cpp: host (windows, Screens, toolbars, host panels and dialogs, shortcuts)
├── EditingWorkspaceUI        workspace_ui.hpp; methods in workspace_ui_*.cpp
│   ├── EditingSession        private; the only mutable session
│   ├── WorkspaceSelection    scene-instance selection
│   ├── EditingViewportUI     MeshEditingUI (MeshToolsUI → MeshMenu, MeshOperationControls),
│   │   │                     MeshNavigationControls, ToolPanel, GizmoSelector, RotationPivotControls,
│   │   │                     BlueprintMeshPanel
│   │   └── ViewportToolsUI   input priority; CameraGizmo, GizmoControls, SelectionInputLogic,
│   │                         SceneMoveGizmo, Instance{Transform,Rotation}Gizmo, MeshTransformGizmo,
│   │                         RegionEditor, SurfacePartTool, ...
│   ├── SceneLists
│   └── TimelineEditingUI → TimelinePanel
└── PreviewLogic              preview_logic.hpp; PreviewSession, PreviewDeliveryLogic, PreviewMailbox
```

**Contracts:**
- **Mode to situation.** The persisted `ViewMode` is a file and protocol value. `workspace_situation(view)`
  (`workspace_situation.hpp`) converts it once into
  `WorkspaceSituation = std::variant<InspectScene, InspectMesh{blueprint}, InspectEffect>`. Situations are ephemeral:
  never store them, and never re-switch on the mode further down.
- **Mutation flows down.** Only `EditingWorkspaceUI` holds a mutable `EditingSession`. Children that observe the
  document borrow a `const EditingSession&` through their constructor. They return proposals (reply values, `take_edit()`),
  and the workspace executes them (`execute(...)`, `apply_pending(...)`, `finish`, `cancel`) and acknowledges the result
  back down. Proposals that continue a gesture carry the session's `active_transaction()`, and the executor rejects
  a stale one, so a late reply cannot end a newer gesture.
- **Handlers.** A component's situation handlers are **private** overloads `handle(const Situation&, const Context&)`
  that return a reply value (`ToolPanel`'s return `void`). Each internal child friends its immediate owner (e.g.
  `friend class EditingViewportUI;` in `mesh_editing_ui.hpp`), which calls `child.handle(…)` directly. There is no base
  class, registry or visitor. A missing handler is a compile error.
- **Dispatch.** `dispatch(component, situation_or_variant, context)` (`component_dispatch.hpp`) holds the one generic
  `std::visit`. Use it to unpack a runtime variant, as at the workspace entry. Only four entry points friend
  `Dispatcher` and are called through it (from `app.cpp` and `workspace_ui_viewport.cpp`): `EditingWorkspaceUI`,
  `ViewportToolsUI`, `SceneMoveGizmo` and `PreviewLogic`. Don't make a new internal child friend `Dispatcher`.
- **Contexts.** Optional sections in a context struct mean "no work for this branch". Context spans are borrowed for
  the call, never stored.
- **Ownership.** Children neither include their parent nor keep parent pointers.
- **Ordered input.** Each frame the host builds `ViewportInputSteps` (`viewport_input.hpp`). For each step it calls
  `workspace.interact_viewport(ViewportInputContext)` and then `workspace.pick_viewport(...)`, so one occurrence
  finishes navigation → tools → picking before the next starts. A final empty tick advances held keys once.
  `ViewportToolsUI` arbitrates (`accepts`, `observe`); `interact_viewport` (`workspace_ui_viewport.cpp`) runs the tools
  and executes their proposals. The replies (`ViewportInputReply`, `ViewportSelectionReply`) tell the host what to
  present or send; they hold no callbacks.
- **Gizmos.** `MoveGizmo<Movement>` (`move_gizmo.hpp`) with the bindings `InstanceMovement`, `RotationOriginMovement`
  and `SurfaceAltitudeMovement`. The parent calls `update`, `append` and `cancel` on the gizmo it selected.
- **Diagnostics.** Owners and most children expose `debug_report()` and nest their children's reports; Logs →
  Component diagnostics shows the tree on demand.

The host still owns toolbars, dialogs, the Settings and Inspector panels, the camera menu, window-wide shortcuts,
worker lifecycle and presentation. It borrows references to some workspace children (`workspace.interaction()`,
`workspace.blueprint_panel()`, `workspace.gizmo_selector()`, …) for wiring and writes private view state through
`workspace.viewport()`, but authors document changes only through workspace methods.

**Before you edit this layer:**
- Put the change in the narrowest owner (a leaf panel or tool, or below the layer), with a `vng_editor_ui_tests` case.
  The patterns are in `workspace_tests.cpp`, `viewport_input_tests.cpp`, `workspace_picking_tests.cpp`,
  `movement_binding_tests.cpp`, `input_routing_tests.cpp` and `component_dispatch_tests.cpp`.
- Keep the prose in step, in the same commit: [docs/editor_components.md](../editor_components.md),
  [docs/editor_boundaries.md](../editor_boundaries.md) ("Viewport interaction ownership"), docs/codebase.md "Editor
  ownership", and the component sections of `docs/explore/codebase-details.js` (sources `workspace_ui.hpp`,
  `workspace_ui_*.cpp`, `move_gizmo.hpp`, `timeline_editing_ui.hpp`, `preview_logic.hpp`, `app.cpp`). Their editor
  excerpts are sketches (`exact` is false), so no test catches drift ([docs-maintenance.md](docs-maintenance.md)).

## 7. Gotchas

- **Pose edits.** `EditingSession::can_edit_scene_pose()` needs a selected keyframe equal to the playhead while paused.
  Native inspector Apply (`begin_remote`) has the same gate.
- **Keyframe identity** is exact `f32` equality of times. In a baked timeline, every distinct key time counts as a
  scene keyframe.
- **Track growth.** `add_keyframe` keys every property of every instance, which creates tracks.
  - Growth is capped by the *preference* "Timeline track limit" (default 256, maximum 16,384). Existing tracks above
    the cap still load and play; only adding tracks fails ("Increase Settings > Timeline track limit").
  - The hard limits are 16,384 keys in total and 4,096 per track ([limits-and-non-features.md](limits-and-non-features.md)).
- **New tracks.** The first key after t = 0 on a new track also seeds a hold key at 0 holding the base value.
- **Selection after clearing.** The default document has instances 1 "Mesh" and 2 "Sun", and `selected_object = 2`.
  `validate_state` rejects a selection that no longer exists, so code that clears instances must reset
  `viewport.selected_object`.
- **Legacy evaluation.** `evaluate_scene(...).model`/`.sun` reflect only instance ids 1 and 2. Use `.instances`.
- **Cameras** are instances.
  - `rotation.x = -pitch`, `rotation.y = yaw`, and roll is ignored. `focus` is the orbit pivot distance.
  - `active_camera(t)` returns the first camera in instance order whose `active` is true, else the first camera.
  - Two cameras active at the same keyed time is invalid.
- **Whole-state copies.** Session timeline operations (`add_keyframe`, `update_keyframe`, …) start from
  `auto candidate = state_`, a copy of the whole `State` with its embedded meshes. Expect them to be slow on the
  40-50 MB scenes (not measured).
- **History.** Undo keeps 64 entries. Load clears history and the clipboard.
- **Instance transforms.** Scale and axis scale must be in [0.05, 1e6], Euler components in [-360, 360] degrees, and
  position within ±1e6. The matrix is `T*Rz*Ry*Rx*S`.
- **Revisions.** A revision identifies the editor session, not the file; the editor overwrites the file's
  `revision` (which must still be valid to load). Opening a scene with `--scene` starts at revision 1. An in-session
  Load keeps the session's revision and advances it.
- **Where docs and code disagree, code wins.** In particular:
  - the writer (`encode_state` in `project.cpp`) decides the saved project version;
  - `animation_properties()` is the list of animatable properties (the editor section of docs/timeline.md predates
    cameras and regions).

## See also
- [README.md](README.md): index of the agent guides.
- [docs/editor.md](../editor.md): user-facing controls, settings table, validation.
- [docs/editor_components.md](../editor_components.md): component ownership, proposals, ordered input, gizmo bindings.
- [status.md](status.md): current branch, refactor and test status.
