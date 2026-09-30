# Status snapshot (volatile, dated)

## Integration update — 2026-09-30

This branch integrates PRs #15 (agent guides and camera keys), #16 (scene review
app) and #17 (workspace, camera gizmos, octagonal tunnels and architecture map).
The 2026-09-28 branch/checkouts/budget records below are historical, not current
instructions. Re-check merge status with `gh pr list --state open` and fetch
`origin/main` before choosing a base.

The animation-forest feature was isolated from the old checkout and integrated
on `feature/scene-animation-forest-merge`, based on the merged PRs above. Check
`git log origin/main -- examples/editor/scene_animation.cpp` for its merge.
The main checkout remains on `feature/scene-animation-forest` with its original
uncommitted files; this integration deliberately leaves those files and its
index untouched. Do not discard or switch it merely to catch up with main.

Current source limits are 196,608 editor-mesh vertices and 180,224 Earth-authoring
vertices. The scene decoded-byte budget remains 64 MiB. The architecture catalog
now covers every CMake library and all non-test executable dependencies.

## Historical snapshot — 2026-09-28

Read this when you choose a base branch, see a failing test, find someone else's worktree or branch, or need to know
how full the big scene or the disk is. **Snapshot: 2026-09-28, `origin/main` at 0b7b5c1; `feature/agent-docs` is
0b7b5c1 + f885dab + the agent-guides commit.** Every fact here will go stale. Re-run its check before you rely on it,
and update or delete lines when they stop being true. Other docs link here instead of repeating these facts.

**First, check that this snapshot is still current** (about 5 s; `git fetch` and `gh` reads need no approval):

```sh
git fetch origin && git log -1 --oneline origin/main       # snapshot: 0b7b5c1
gh pr list --state open --json number,title,headRefName    # snapshot: [] (the feature/agent-docs PR not yet opened)
git cat-file -e origin/main:AGENTS.md 2>/dev/null && echo "main has the guides" || echo "main lacks the guides"   # snapshot: lacks
git --no-optional-locks -C /home/luke/Desktop/GameAndEngine branch --show-current   # snapshot: feature/architecture-map
```

If any output differs, treat Branches and Checkouts below as stale and choose the base yourself
([collaboration.md](collaboration.md#choose-a-base)).

**TL;DR**
- `main` (0b7b5c1) has PR #13 (departure cinematic, `KeyBatch`, review tools) and PR #14 (editor parent-coordination
  refactor). No PR is open. Base new work on `origin/main`.
- `main` lacks f885dab (the `KeyBatch` camera-pitch fix) and the agent guides. Both are on local `feature/agent-docs`,
  to be proposed as one PR. Until it merges, read the guides from that branch (Checkouts).
- No known test failures: node tests 18 of 18 on both; the quick suites listed under Tests pass on `feature/agent-docs`.
- The main checkout holds another session's uncommitted editor and explore-guide work, including settings format 9.
- The departure scene uses 56.1 of 64 MiB decoded and 10,131 of 16,384 keys. The disk has 9.5 GB free.

## Branches and PRs

| Ref | Commit | What it is |
|---|---|---|
| `main`, `origin/main` | 0b7b5c1 | PR #14 merge, on top of the PR #13 merge cb9375a (both 2026-09-27 19:45). Has the cinematic, `KeyBatch`, the review tools and the editor refactor. **Lacks** f885dab and the guides. |
| `feature/agent-docs` (local, no upstream at snapshot) | f885dab, then the guides commit | f885dab "Keep batch-keyed rotations that already fit instead of adding a turn" (parent 0b7b5c1): `key_batch.cpp`, the regression test in `tunnel_scene_tests.cpp`, and the regenerated `tunnel_departure.vscene` (same pictures; the stored angles lose the extra turn). Then `AGENTS.md`, `CLAUDE.md`, `docs/agents/` and a `docs/tunnel.md` numbers fix. Checked out in `.claude/worktrees/voyage`. To be proposed as one PR against `main`. |
| `wip/voyage-next` (local, no upstream) | 3b9cc8e | **Obsolete.** 63f4019 (the same change as f885dab, on the pre-#14 base a8aff7d) plus an early WIP commit of the guides. Everything in it is on `main` or `feature/agent-docs`. Not checked out. Don't base on it; deleting it is the owner's call. |
| `feature/architecture-map` (local, no upstream) | 0b7b5c1 | No commits of its own yet. Checked out in the main checkout. |
| `feature/earth-skyways-departure-cinematic` (local) | e4e4bb3 | Merged as PR #13; the local branch is 3 behind its origin (a8aff7d). Not checked out. |
| `refactor/editor-parent-coordination` | bc4e84f | Merged as PR #14. Not checked out. |
| PR #12, **closed unmerged**; `refactor/remove-legacy-animation-camera` | 47203a4 | In no base. The legacy animation camera (`pilot_camera`, `animation_camera`) still exists everywhere. |
| `fix/keyframe-inspector-reveal`, `refactor/editor-context-tree`, `claude/modest-ellis-502f2c` | deaf5bd | No commits of their own. Owners unknown; leave them alone. |

**Choosing a base.** Use `origin/main`. If you batch-key a camera that looks level or down all shot (an orbit above
its subject, a chase from above), you also need f885dab
([scenes-and-cinematics.md §6](scenes-and-cinematics.md#6-traps)). Ask the owner whether the `feature/agent-docs` PR
lands first, or whether you carry the fix: `git cherry-pick f885dab` applies cleanly to 0b7b5c1 (its parent). Name it
under `## Notes` in your PR either way.

**Departure scene conflict.** f885dab rewrites `examples/assets/tunnel_departure.vscene` (4,416 changed lines of a
48.5 MB file). A branch off `main` that also regenerates that scene conflicts with it. Ask the owner which lands first
before you `--replace` it. After any merge, regenerate the scene with `vng_make_tunnel_departure ... --replace` instead
of resolving the file by hand.

```sh
git branch -vv
git log --oneline origin/main..feature/agent-docs   # what the branch adds: f885dab, then the guides
git show origin/main:examples/scenes/key_batch.cpp | grep -c 'high > 360.F ?'   # 1 once main has the fix
```

## Checkouts, worktrees and stashes

- **Main checkout** `/home/luke/Desktop/GameAndEngine` is on `feature/architecture-map` (0b7b5c1) with another
  session's uncommitted work: 42 changed paths on 2026-09-28 (31 modified, 1 deleted, 10 new). Don't touch it.
  - An `architecture` page for the explore guide: `docs/explore/architecture*`, a new
    `docs/explore/tests/architecture.test.cjs` that `CMakeLists.txt` adds to `vng_guide_tests`, and edits to most
    other `docs/explore/` files, `docs/editor.md` and `docs/editor_components.md`.
  - An editor camera gizmo: new `examples/editor/camera_gizmo.{hpp,cpp}` (`CameraGizmo`, `CameraPointerGizmo`,
    `CameraWalkGizmo`) and `tests/editor/camera_gizmo_tests.cpp`. It **deletes** `camera_navigation_logic.hpp`
    (`CameraNavigationLogic`) and edits `camera_*`, `viewport_*`, `workspace_ui*`, `app.cpp` and editor tests.
  - **Editor settings format version 9** (`camera_drag.zoom` in `settings.cpp`, plus `settings_panel.hpp`,
    `camera_preferences.hpp`, `camera_limits.hpp`, `tests/editor/settings*_tests.cpp`). A preference you add would
    claim the same version number ([editor.md](editor.md#add-an-editor-preference)).
  - Work on any of these files collides with it: ask the owner first
    ([collaboration.md](collaboration.md#check-for-in-flight-edits-to-your-files) has the check).
  - When it lands, these guide statements go stale: [editor.md](editor.md) §2 and §6 (`camera_navigation_logic.hpp`,
    `CameraNavigationLogic`) and "Add an editor preference" (`if (version8)` as the newest block);
    [docs-maintenance.md §2](docs-maintenance.md#2-what-vng_guide_tests-enforces) (12 content tests, exactly 5 pages,
    17 role headers). Check: `git log --oneline origin/main -- examples/editor/camera_gizmo.hpp`.
- **Where to read the guides until `main` has them.** A worktree based on `main` has no `AGENTS.md` or `docs/agents/`.
  - Read them from the branch, from any worktree: `git show feature/agent-docs:docs/agents/<file>.md`, or in
    `/home/luke/Desktop/GameAndEngine/.claude/worktrees/voyage/`. Don't copy them into your branch unless the owner
    asks.
  - When a guide tells you to edit a guide (a table row, this file), put the exact edit under `## Notes` in your PR
    and in your report instead.
  - Briefs for delegates must carry the rules themselves ([delegating.md §1](delegating.md#1-the-brief)).
- `.claude/worktrees/voyage` has `feature/agent-docs` checked out. It belongs to the session that wrote the guides.
  Don't stage, commit, switch or edit there; make your own worktree.
- `.claude/worktrees/modest-ellis-502f2c` is detached at deaf5bd, with a clean tree. Owner unknown; don't remove it.
- `.claude/worktrees/` also holds other sessions' output folders and files that are not worktrees. Read them if
  useful; don't delete or write into them without asking.
- **6 prunable worktrees** are registered under `/tmp`; their directories are gone.
  - `refactor/remove-legacy-animation-camera` is still registered to one of them, so `git switch` to it fails with
    "'refactor/remove-legacy-animation-camera' is already used by worktree at ..." (git 2.43).
  - `git worktree prune` fixes that, but it changes shared state. Ask first.
- Stashes `stash@{0}` ("Before PR 11 integration: ...") and `stash@{1}` ("Before PR 10 integration: ...") are the
  owner's safety stashes. Never pop or drop them. They also show up in `git log --all` as "On main: ..." commits.

```sh
git worktree list
git stash list
git --no-optional-locks -C /home/luke/Desktop/GameAndEngine status --short | wc -l   # read-only; 42 at snapshot
ls -la /home/luke/Desktop/GameAndEngine/.claude/worktrees/
```

## Tests

| Fact | Check |
|---|---|
| CTest registers 91 tests. The pre-PR command (`-E '^vng_(window_glfw\|x11_scroll)_tests$'`) runs 89; the no-window command runs 86. | `ctest --test-dir build -N \| tail -1` |
| `vng_guide_tests` (12 of 12) and `vng_layering_tests` (6 of 6) pass on `main` and `feature/agent-docs`: 18 of 18. | `node --test tests/layering/layering.test.cjs docs/explore/tests/content.test.cjs` (0.2 s, no build) |
| On `feature/agent-docs` (2026-09-28) these pass: `-L compile-fail` (42 of 42), `vng_editor_tests` (353 cases), `vng_editor_ui_tests` (273 cases, 16 s), `vng_glfw_opengl_tests "~[modes]"`, `vng_tunnel_scene_tests` (5 cases, 9,928 assertions) and `vng_tunnel_render_tests`. No known failures. | the commands in [build-and-test.md §4](build-and-test.md#4-run-tests) |
| Full ctest has not been run since e4e4bb3 (90 of 91 then; the one failure was the guide excerpt PR #14 fixed). Take your own baseline on your base. | `ctest --test-dir build -j 12 --output-on-failure` (~4 min; needs a long tool timeout) |
| `vng_tunnel_scene_tests` has 5 cases on `feature/agent-docs` (the fifth is the `KeyBatch` regression test) and 4 on `main`. | `./build/vng_tunnel_scene_tests --list-tests \| tail -1` |

## Scene budgets in use

Limits and their sources: [limits-and-non-features.md](limits-and-non-features.md).

| Asset | Usage at snapshot | Check |
|---|---|---|
| `examples/assets/tunnel_departure.vscene` | On `feature/agent-docs`: file 48.5 MB (48,491,063 bytes); **56.1 of 64 MiB decoded**; 423 instances, 18 blueprints, 909 tracks; **10,131 of 16,384 keys**; longest track 1,959 of 4,096. Byte-identical to fresh `vng_make_tunnel_departure` output there. `main`'s copy differs only in stored angles. | `./build/vng_scene_probe examples/assets/tunnel_departure.vscene --size` (~20 s) |
| `examples/assets/earth_future.vmesh` | 117,466 of 131,072 vertices (13,606 left: roughly 10 more 32-segment global corridors at ~1.3k vertices each; per-part costs in [assets.md](assets.md#vertex-budget)) | `grep -m1 '^vertices' examples/assets/earth_future.vmesh` |
| Other editor scenes (files unchanged since 40eaf29; measured then) | earth_future 43.5 MiB decoded; asteroid_fleet 23.9; solar_system 19.6; earth and earth_savannah 14.7; fleet_reveal 7.4; tunnel_interior 3.7 | `--size` on each; `git log --oneline -- examples/assets/<file>` |
| Hand-edited committed scenes | asteroid_fleet has 913 instances (its generator makes 642). solar_system has 644, the same count its generator makes (4 + 40 ships + 600 rocks, `solar_system_scene_tests.cpp`); whether it holds other hand edits is unverified. `fleet_reveal`, `asteroid_fleet`, `earth`, `earth_savannah` and `editor_timeline` have no camera instance (legacy camera). **Regenerating destroys hand edits.** | `./build/vng_scene_probe SCENE --list` |

## Build directories

- `.claude/worktrees/voyage/build`: Debug, a full build at f885dab (4.3 GB), current for that tree.
  - Of the EXCLUDE_FROM_ALL tools, only `vng_make_tunnel_departure`, `vng_scene_frames` and `vng_scene_probe` are built.
  - Other agents have used this directory. Don't run `cmake --build` in it; build in your own worktree.
  - Check: `ls -l build/vng_make_* build/vng_scene_*`.
- That build takes its Catch2, GLAD, GLFW and HarfBuzz sources from the main checkout's `build/_deps/*-src`.
  Deleting the main build breaks reconfiguring. Check: `grep FETCHCONTENT_SOURCE_DIR build/CMakeCache.txt`.
- The main checkout's `build/editor-e2e` holds 450 e2e runs (6.3 GB). Pass `--artifacts "$S/e2e"` when you run
  `vng_editor_e2e_tests` by hand.
- A build prints many pre-existing warnings. On 2026-09-28, rebuilding 148 files after PR #14 logged 366 `warning:`
  lines (287 in `tests/editor`, 44 in Catch2 headers, 23 in `examples/editor`). They are not yours. Filter for your
  own files ([build-and-test.md §2](build-and-test.md#2-build)).

## Machine and agent environment

- 28 cores and 31 GiB RAM. Several agents building at `-j 20` at once can exhaust RAM
  ([build-and-test.md](build-and-test.md#2-build) has the `-j` rule).
- **Free disk is tight.** `/` (which holds `/home`) is 480 GB, with 9.5 GB free (98% used) late on 2026-09-28. It
  read 7.7 GB earlier that day and 11 GB the day before.
  - Build dirs: the main checkout's 12 GB (6.3 GB of it e2e artifacts), voyage 4.3 GB, `modest-ellis-502f2c` 3.9 GB.
  - A full Debug `build/` is ~4.3 GB; one with only `vng_make_tunnel_departure` ~0.4 GB; with the GL review tools and
    the tunnel test ~0.9 GB (estimate). A checkout is ~0.22 GB. A 960x600 frame is ~0.2 MB (1,000 frames ≈ 200 MB).
  - Check before you configure a new build dir, and tell the owner if it would leave less than ~5 GB:
    `df -h /home; du -sh /home/luke/Desktop/GameAndEngine/build /home/luke/Desktop/GameAndEngine/.claude/worktrees/*/build`.
- Installed: g++ 14.2.0, clang++ 18.1.3, cmake 3.30.9, node 18.19.1, ffmpeg, python3 with PIL 10.2 and numpy 2.3.5, gh and Xephyr.
- **No `xvfb-run`.** `DISPLAY=:1` is the owner's real desktop.
- Check: `nproc; free -g; g++ --version | head -1; node --version; command -v xvfb-run || echo "no xvfb-run"; echo "$DISPLAY"`.
- Claude memory with the owner's preferences exists only under `~/.claude/projects/-home-luke-Desktop-GameAndEngine/memory/`.
  The worktree project keys have no `memory/` directory, yet Claude Code sessions in `.claude/worktrees/voyage` were
  still shown that memory on 2026-09-27 and 2026-09-28. Codex never sees it. Rely on `AGENTS.md` and
  [collaboration.md](collaboration.md#6-the-owners-preferences-in-full), which carry the same preferences.
- There is no CI and there are no git hooks. Check: `ls .github; git config core.hooksPath; ls "$(git rev-parse --git-common-dir)/hooks" | grep -v sample`.

## Known doc drift

Report these; don't fix them in an unrelated PR without asking. Checked on `feature/agent-docs`, 2026-09-28.

| Doc | Stale spot | Truth (source) |
|---|---|---|
| `docs/tunnel.md` | Budget numbers and route name: fixed on `feature/agent-docs`, still stale on `main` until it merges. Still stale everywhere: "Earth retains its own illustrated material" (section "One tunnel, viewed from either side"). | In the departure, Earth uses `render/lighting = tunnel_departure_night` (`examples/scenes/tunnel_departure.cpp`). Nothing outside `docs/agents/` links to tunnel.md. |
| `docs/codebase.md` (~:47) | "exactly one target of the same name" | A table: `core`/`input` → `vng_core`, `window` → `vng_window_glfw`, preview files → `vng_editor_preview` (`moduleTargets`/`fileTargets` in `tests/layering/layering.test.cjs`) |
| `docs/asteroid_fleet.md` (~:25) | "641 instances" | The generator makes 642 (`tests/examples/asteroid_scene_tests.cpp`); the committed file has 913 |
| `docs/fleet_reveal.md` (~:51), `docs/asteroid_fleet.md` | "the Tracking camera is a scene camera instance" | True only after regeneration. The shipped files use the legacy camera. |
| `docs/timeline.md` (editor bridge, ~:84-105) | Only Mesh (1)/Sun (2) targets; camera tracks "not part of this slice" | Per-instance properties for every kind (`animation_properties`). It also never states the 4,096/16,384 key limits. |
| `docs/editor.md` (~:667) | "Saved editor projects now use version 2" | The writer emits `editor_project = 4` |
| `docs/editor.md` (~:474-480) | The `environment` field list | Omits `view_distance` (`examples/editor/environment.hpp`) |
| `docs/earth.md` (~:28, ~:423, ~:723) | outer corridors "use 24 segments"; ownership list stops at 6; "no ... city lights ... yet" | 32 segments (`examples/scenes/earth_network.cpp`); 7 = processor (`InfrastructureKind`); earth_future has night lights. It also doesn't warn that `vng_make_earth --tunnel-classes` is unsafe on the current `earth_future`. |
| `README.md`; `examples/README.md` | No `vng_tunnel_demo`, `vng_make_*`, `vng_scene_frames` or `vng_scene_probe`. README also lacks the earth and solar_flyby demos. | `CMakeLists.txt` |
| `docs/documentation_work.md` (~:5, ~:16) | "33 real components"; "All 11 content tests ... pass" | 35 components; 12 content tests, all passing |
| `docs/explore/README.md` (~:55) | says `index.html` excerpts are checked | Only `demo.html` has `data-source` excerpts |
| `docs/explore/maintenance.md` (~:174) | Playwright path under `/tmp` | Gone (the Chromium build under `~/.cache/ms-playwright/` remains). Installing Playwright needs the owner's permission. |

The main checkout's uncommitted work edits `docs/editor.md`, `docs/editor_components.md` and `docs/explore/`, so
check its diff before you fix a row in those files.

## Known code issues

| Issue | Check |
|---|---|
| **`KeyBatch::commit` can point a batch-keyed camera straight up** (whole-turn shift of all-negative rotation components; details in [scenes-and-cinematics.md §6](scenes-and-cinematics.md#6-traps)). **Fixed by f885dab** on `feature/agent-docs`. `main` (0b7b5c1) still has the bug until that branch merges. | `git show origin/main:examples/scenes/key_batch.cpp \| grep -n -A1 'const auto shift'` (the fixed line starts `high > 360.F ?`) |
| **The courier points ~41° off its flight on the 59 s cut.** Its rotation keys at 58.999 and 59.0 s are (12.65, 41.1, −7.28)°, against (−0.84, 45.47, −7.71)° at 58.967 and (0.14, 0.01, −5.48)° at 59.011: the voyage's `heading` differences `position` across the flyby jump at `flyby_end` ([scenes-and-cinematics.md §5.7](scenes-and-cinematics.md#57-other-techniques-in-the-voyage-departure_voyagecpp)). One wrong frame on the cut, and a roll disturbance until ~59.3 s. | `grep -n 'const auto heading' examples/scenes/departure_voyage.cpp` |
| Nit: `examples/support/earth_placement.hpp` has a namespace-scope `using namespace vng;` in a header. The convention (commit ef9e8ea) removed those. | `grep -n 'using namespace' examples/support/earth_placement.hpp` |
