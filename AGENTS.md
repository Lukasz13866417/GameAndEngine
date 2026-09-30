# Agent instructions for vng

Read this at the start of every session. Codex loads it directly; Claude Code loads it through `CLAUDE.md`.
Topic guides: [docs/agents/README.md](docs/agents/README.md). Volatile, dated facts (open PRs, known test failures,
who is editing what, current scene sizes) live only in [docs/agents/status.md](docs/agents/status.md).

**TL;DR**
- Work in your own git worktree on your own branch. Never touch the main checkout.
- Build the one target you need, then run that test binary with a filter. `ctest` never builds.
- Ask in chat before you push, open a PR, open windows on the desktop or regenerate committed assets unrequested.
- Visual work: probe, then frames, then a sheet or MP4. No camera shake. Offer variants when a choice is unclear.
- Code and `CMakeLists.txt` are the truth. Docs, these included, are snapshots.

## 1. What this is
- `vng` ("Vibe Engine") has three parts: C++23 engine libraries (`include/vng`, `src`), a two-process Linux/OpenGL 4.6
  editor (`vng_editor_demo`), and offline scene generators with cinematic demos. It needs CMake 3.25 or newer.
- The owner is Lukasz (GitHub `Lukasz13866417`); use they/them. They review work as GitHub PRs. Other agents
  (Claude, Codex) often work on the same machine at the same time.
- A session in a worktree may load two copies of these instructions (the main checkout's and its own). If they
  differ, follow the copy in your working tree. Don't rely on agent memory or on notes kept outside the repo.
- The owner's architecture preferences: obvious component roles (several small components beat one hub); one owner
  and a clear lifecycle per mutable resource; tree-like dependencies that are easy to spot, not a web of cross-links
  (the layering test only enforces "acyclic"); local changes cause local work; little boilerplate; backend-neutral
  code never depends on OpenGL or GLFW. The full list: [collaboration.md](docs/agents/collaboration.md).

## 2. Safety rules
- **The main checkout is not yours** (`/home/luke/Desktop/GameAndEngine`, or in general
  `dirname "$(git rev-parse --path-format=absolute --git-common-dir)"`). It usually holds another agent's uncommitted
  work. Never edit, stage, stash, switch or build there unless the owner asks in chat. Nor in any worktree you didn't
  create, even the one you start in, if `git status --short` shows others' changes: read only, make your own (§3).
- Keep worktrees, outputs and any work you can't lose out of `/tmp`; a reboot there has wiped hours of work. Commit
  WIP early. Put renders and deliverables in the main checkout's `.claude/worktrees/<task>.out/` (the guides' `$S`).
- Don't use `git stash` at all (no push, pop, drop or clear): every worktree shares one stash stack, and it holds the
  owner's safety stashes. Make a WIP commit instead. Never `git worktree prune` or delete other people's branches or
  worktrees without asking.
- Commit only your own hunks. Never sweep the owner's or another agent's uncommitted changes into a commit.
- `git push`, creating or editing a PR, and publishing anything all need approval in chat. If a permission guard
  denies an action, explain and ask. Do not work around it.
- Never regenerate or re-save committed `examples/assets/*.vscene` or `*.vmesh` files that the owner's request doesn't
  cover. Generate into `$S` first either way. Several hold hand edits that no generator reproduces.
- Never run `cmake --build` in a build dir that another agent uses. Use your own worktree's `build/`.
- `DISPLAY` is the owner's real desktop (there is no xvfb). Every graphical `vng_*_demo` (all but
  `vng_document_demo`, even with `--screenshot`), `vng_editor_demo`, `vng_window_glfw_tests` and
  `vng_x11_scroll_tests` (Xephyr) open **visible** windows: avoid them unless the task needs one. Briefly visible:
  `vng_glfw_opengl_tests` (`[modes]` goes fullscreen), and Independent Play in `vng_editor_worker_tests` and the
  `--popout` e2e run. `vng_scene_frames` and the other GPU and e2e tests stay hidden.
- Running the editor: set `XDG_CONFIG_HOME="$S/config"` (else it rewrites `~/.config/vng/editor.settings`) and open
  a *copy* of the scene (Save/Ctrl+S overwrite the opened file). More in editor.md.
- Ask before you install anything or use the network beyond `git fetch` and `gh` reads.

## 3. Choose a base and make a worktree
- `git fetch origin` first (no approval needed). Base on `origin/main` unless you need work it lacks (`gh pr list`,
  `git branch -a --contains <commit>`); then ask the owner, or stack on the newest branch that has it. Unpushed local
  commits: [collaboration.md §2](docs/agents/collaboration.md#choose-a-base). Name the base in the PR. Today: status.md.
- A base without `AGENTS.md` or `docs/agents/` gives you no guides: status.md says where to read them. Don't copy them.
- Name branches `feature/`, `fix/` or `refactor/` plus a kebab-case slug. A branch lives in one worktree at a time.
```sh
MAIN="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")"
git -C "$MAIN" worktree add -b feature/<slug> "$MAIN/.claude/worktrees/<name>" <base>   # git-excluded dir
cd "$MAIN/.claude/worktrees/<name>"
```

## 4. Build (from the worktree root)
```sh
M="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")/build/_deps"   # sources the main build fetched
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DFETCHCONTENT_SOURCE_DIR_CATCH2=$M/catch2-src -DFETCHCONTENT_SOURCE_DIR_GLAD=$M/glad-src \
  -DFETCHCONTENT_SOURCE_DIR_GLFW=$M/glfw-src -DFETCHCONTENT_SOURCE_DIR_HARFBUZZ=$M/harfbuzz-src \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build --target <target> -j 12
```
- If `$M` does not exist, ask first: without the `FETCHCONTENT_*` flags CMake downloads the pinned sources.
- Always pass `-DCMAKE_BUILD_TYPE`; the README's bare configure gives no `-g` and no `-O`.
- Build named targets; a full Debug build takes minutes. Use `-j 12` at most, `-j 8` while other agents build.
- After adding a target to `CMakeLists.txt`, re-run `cmake -S . -B build`, or you get "No rule to make target".
- `vng_make_*`, `vng_scene_frames` and `vng_scene_probe` are `EXCLUDE_FROM_ALL`. Name them explicitly, and rebuild
  them after changing code they use (e.g. `examples/editor/mesh_shading.hpp`), or you run stale binaries.
- A build dir is tied to its source tree (asset paths are compiled in). Run `./build/...` from your own worktree.

## 5. Test
- `ctest` never builds. Rebuild the test target, then run the binary with a Catch2 filter, either a tag
  (`./build/vng_timeline_tests "[sample]"`) or one source file
  (`./build/vng_editor_tests -# "[#picking_acceleration_tests]"`). A filter typo exits 2 with "No tests ran".
- No build needed: `node --test tests/layering/layering.test.cjs docs/explore/tests/content.test.cjs` (<1 s).
- **"Skipped" is not "passed".** Without an OpenGL 4.6 context, GPU tests exit 77 and CTest still prints "100% tests
  passed", so read the skip list. Quick hidden check, once built: `./build/vng_glfw_opengl_tests "~[modes]"` must exit 0.
- `ctest -L editor` misses the CPU editor tests. Use `-R 'vng_editor_'` (includes the slow e2e runs) or names.
- Before a PR: `cmake --build build -j 8` and the EXCLUDE_FROM_ALL tools you used (ctest fails missing test binaries
  as "Not Run"), then `ctest --test-dir build -j 12 --output-on-failure -E '^vng_(window_glfw|x11_scroll)_tests$'`
  (~4 min). Three of its tests flash a window: ask first, or use build-and-test.md §8's no-window run.
- Agent shells often time out at 2 min. Pass an explicit timeout (up to 10 min) or run in the background for full
  ctest, `-L e2e` (~2.3 min), `vng_editor_runtime_tests` (~90 s), `vng_earth_assets_tests` (~70 s) and full builds.
- Which tests to run for which change: [build-and-test.md](docs/agents/build-and-test.md).

## 6. Repo map
| Path | Contents |
|---|---|
| `include/vng/<m>/`, `src/<m>/` | Engine libraries ([engine.md](docs/agents/engine.md)). Directory to target: `moduleTargets` in `tests/layering/layering.test.cjs` |
| `examples/editor/` | Editor app ([editor.md](docs/agents/editor.md)). Model: `project.hpp` (`State` = `Document` + `ViewportState`); edits: `EditingSession` |
| `examples/scenes/` | Scene generators (`author_scene`), `KeyBatch` bulk keying, demo glue ([scenes-and-cinematics.md](docs/agents/scenes-and-cinematics.md)) |
| `examples/support/` | CPU asset recipes (Earth, ships, space assets, sun) and demo helpers |
| `examples/tools/` | `vng_make_*` generators, `vng_scene_frames`, `vng_scene_probe` |
| `examples/assets/` | Committed meshes and scenes, in plain git (some are 20-50 MB) |
| `examples/*.cpp` | Demos (`vng_*_demo`). `editor.cpp` is the UI process (`vng_editor_demo`), `editor_worker.cpp` the preview worker |
| `tests/` | Catch2 suites, compile-fail tests (`tests/compile_fail`), layering test |
| `docs/*.md`, `docs/explore/` | Topic docs ([docs/codebase.md](docs/codebase.md) is the source tour), and an offline HTML guide whose code excerpts are drift-tested |

## 7. Invariants that break the build or tests
- **Layering.** Every `include/vng`/`src` directory maps to a target in `moduleTargets`. Include `<vng/x/...>` only
  from targets that link `x`. Never create an include cycle or reach into another directory's private headers.
- **Includes in `examples/`.** Cross-directory includes use the relative `"../dir/file.hpp"` form, which is the only
  form the checker sees. `support` must not include `editor` or `scenes`; `editor` must not include `scenes`.
- **Guide drift.** If you change code quoted in `docs/explore/` (`*.js`, `demo.html`), the `target_link_libraries`
  of a guide-mapped target, or an editor role class that `content.test.cjs` lists (`docs/editor_components.md` must
  name each), update the guide. See [docs-maintenance.md](docs/agents/docs-maintenance.md).
- **Warnings.** The flags are `-Wall -Wextra -Wpedantic -Wconversion`, with no `-Werror`. Keep code conversion-clean.
  Write floats as `1.0F`. There is no formatter, so match the file you edit.
- **Errors.** Recoverable failures return `std::expected` (or `content::Result`) with a module Diagnostic. Programmer
  errors throw `std::logic_error` or a subclass (mostly `std::invalid_argument`).
- **Outputs.** PNG writers open with `"wbx"`, so render into a new directory. Scene generators need `--replace` to
  overwrite. `vng_make_fleet` and `vng_make_spaceship` overwrite silently. See [assets.md](docs/agents/assets.md).
- **Scene budgets.** Document file ≤ 64 MiB − 256 B, decoded ≤ 64 MiB; timeline ≤ 16,384 keys, ≤ 4,096 per track;
  mesh ≤ 131,072 vertices. Measure with `./build/vng_scene_probe <scene.vscene> --size`. All limits, and what does
  not exist (no mesh transparency, no textures in scenes, no MSAA, no motion blur, no camera roll):
  [limits-and-non-features.md](docs/agents/limits-and-non-features.md).
- **Honest docs.** Never describe planned work as implemented. Never change runtime code to make a doc true; report
  the mismatch.

## 8. Visual and cinematic work
- The owner's taste: natural, never forced; detailed, but not excessively. **No camera shake**, and that includes
  whips or nods in the camera's rotation.
- Check numbers first, then pictures. None of these open a window:
```sh
./build/vng_scene_probe <scene.vscene> --size                    # budgets (~20 s on a 50 MB scene)
./build/vng_scene_probe <scene.vscene> <instance-id> <t> [<t>...] # camera and subject screen positions
./build/vng_scene_frames <scene.vscene> <new-dir> 960 600 --range <from> <to> 60   # ~7.5 s load, ~0.04-0.05 s/frame
```
- Look at the frames yourself. Then make a contact sheet and a 60 fps MP4, with a 1/3-speed copy for fast moments.
  Commands are in [rendering-and-review.md](docs/agents/rendering-and-review.md).
- **Reviewer agents.** After each substantial iteration, give sheets and full frames to separate reviewers:
  cinematography and editing, art direction and lighting, VFX and continuity. Triage their suggestions, because they
  contradict each other. Report what you adopted, what you rejected, and why. How to brief them:
  [delegating.md](docs/agents/delegating.md).
- **Unclear creative choices.** Tell the owner and show 2-3 labelled variants side by side, with a recommendation.
  Keep working on a default until they pick.

## 9. Commits and PRs
- **Subject:** an imperative sentence of about 70 chars that describes the change in behaviour. No trailing period
  and no `type(scope):` prefix. Example: "Surge the courier through the tunnel exit".
- **Body:** wrap at about 72 columns. Say what changed and why, with concrete numbers, and name any known failures.
- **Trailer:** your harness's attribution; Claude Code uses `Co-Authored-By: Claude <model> <noreply@anthropic.com>`.
- **PR body:** `## Summary` bullets, `## Notes` (known failures, large files), and `## Test plan` or a `Verified:`
  line (exactly what ran, with counts and any skipped GPU tests). Claude Code ends it with
  `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- There is no CI and there are no git hooks. Your own test run and the owner's review are the only gates.
