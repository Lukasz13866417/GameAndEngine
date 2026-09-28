# Agent guides: index

Read this when you have read [AGENTS.md](../../AGENTS.md) and need depth on the area you are about to touch.

**TL;DR**
- Start with [AGENTS.md](../../AGENTS.md) for the rules. Then read [status.md](status.md) for what is true today.
- Then read the guide for your area (table below), followed by the existing topic doc it points to.
- These guides add agent know-how: commands, gotchas and recipes. They link to the `docs/*.md` topic docs instead of
  repeating them.
- Only `status.md` holds dated facts. If a guide disagrees with the code, the code wins; fix the guide in your PR
  (see "Keeping these guides true" below).

## The guides

| Guide | One line |
|---|---|
| [build-and-test.md](build-and-test.md) | Configure and build options, the offline configure, test labels and their gaps, Catch2 filters, durations, which tests to run for which change, e2e artifacts, sanitizers |
| [collaboration.md](collaboration.md) | Worktrees and the `$S` output folder, choosing a base, stash and prune hazards, approvals, commit and PR style with examples, the owner's preferences in full, how to report back |
| [delegating.md](delegating.md) | Handing work to sub-agents and other models: what a brief must contain, keeping them isolated, and triaging what comes back |
| [engine.md](engine.md) | Engine library map, CPO+ADL backend dispatch, errors and lifetimes, the shader path (DSL, IR, GLSL), recipes for new modules and GPU resources, gotchas |
| [editor.md](editor.md) | The two editor processes, `State = Document + ViewportState`, `EditingSession` gates, worker packets, recipes (new property, preference, shortcut, panel), running the editor safely |
| [scenes-and-cinematics.md](scenes-and-cinematics.md) | Scene generators and `vng_make_*` tools, `KeyBatch`, cuts and holds, camera conventions and gimbal lock, float precision staging, key density |
| [rendering-and-review.md](rendering-and-review.md) | How a `.vscene` becomes pixels, lighting styles, the two camera paths, and the review loop (probe, frames, contact sheet, MP4) with costs |
| [assets.md](assets.md) | What is in `examples/assets`, which tool made each file, overwrite rules, Earth recipes and the vertex budget, repo growth |
| [docs-maintenance.md](docs-maintenance.md) | What the guide drift tests enforce, the list of quoted files, the house style for `docs/*.md`, linking rules |
| [limits-and-non-features.md](limits-and-non-features.md) | Every numeric limit with its source, and what does not exist (so don't go looking for it) |
| [status.md](status.md) | Dated, volatile facts (branches, open PRs, known failures, who is editing what), each with the command that re-checks it |

## Read X when you touch Y

Paths are relative to the repo root. The existing topic docs are in `docs/` unless noted.

| You touch | Read (agent guide) | Then (existing docs) |
|---|---|---|
| `CMakeLists.txt`, `cmake/`, test registration, a new target | [build-and-test.md](build-and-test.md) | [README "Build and test"](../../README.md#build-and-test), [cmake/glfw/README.md](../../cmake/glfw/README.md) |
| `include/vng/*`, `src/*` (engine) | [engine.md](engine.md) | [codebase.md](../codebase.md), then the topic doc for the module (for example [renderer_api.md](../renderer_api.md), [shader_vertex_pipeline.md](../shader_vertex_pipeline.md), [resources.md](../resources.md)) |
| `include/vng/timeline`, `content`, `gfx` data types | [engine.md](engine.md), [limits-and-non-features.md](limits-and-non-features.md) | [timeline.md](../timeline.md), [documents.md](../documents.md), [mesh_and_vmesh.md](../mesh_and_vmesh.md) |
| `include/vng/text`, `ui`, editor panels and widgets | [engine.md](engine.md), [editor.md](editor.md) | [text_rendering.md](../text_rendering.md), [ui.md](../ui.md) |
| `examples/editor/`, `examples/editor.cpp`, `examples/editor_worker.cpp`, `src/editor/` | [editor.md](editor.md) | [editor.md](../editor.md), [editor_boundaries.md](../editor_boundaries.md), [editor_components.md](../editor_components.md), [editing_session.md](../editing_session.md) |
| Regions, or a new kind of instance | [editor.md](editor.md) | [regions.md](../regions.md) |
| `examples/scenes/`, `examples/tools/make_*_scene.cpp`, `make_tunnel_departure.cpp` | [scenes-and-cinematics.md](scenes-and-cinematics.md) | [tunnel.md](../tunnel.md), [fleet_reveal.md](../fleet_reveal.md), [asteroid_fleet.md](../asteroid_fleet.md), [solar_system.md](../solar_system.md) |
| `examples/editor/runtime.*`, `mesh_shading.hpp`, lighting, the look of a shot | [rendering-and-review.md](rendering-and-review.md) | [art_direction.md](../art_direction.md), [sun.md](../sun.md), [bloom.md](../bloom.md) |
| Reviewing a shot (`vng_scene_frames`, `vng_scene_probe`, demos) | [rendering-and-review.md](rendering-and-review.md) | the shot's own doc, e.g. [tunnel.md](../tunnel.md) |
| `examples/assets/`, `examples/support/*`, `make_earth`, `make_fleet`, `make_spaceship` | [assets.md](assets.md) | [earth.md](../earth.md), [mesh_and_vmesh.md](../mesh_and_vmesh.md) |
| `docs/`, `docs/explore/`, `README.md`, or code that the guide quotes | [docs-maintenance.md](docs-maintenance.md) | [documentation_work.md](../documentation_work.md), [explore/README.md](../explore/README.md) |
| Branches, commits, PRs, other agents' work | [collaboration.md](collaboration.md) | none |
| Spawning reviewers or parallel workers | [delegating.md](delegating.md) | none |
| "Does X exist?" or "What is the limit on Y?" | [limits-and-non-features.md](limits-and-non-features.md) | none |

## Keeping these guides true
- No test checks `docs/agents/`. When you change behaviour that a guide describes, update the guide in the same PR.
  If your base lacks the guides ([status.md](status.md) says whether `main` has them yet), put the exact correction
  under `## Notes` in your PR instead.
- Facts that will rot go only in [status.md](status.md). The house style for agent docs (durable rules, dated facts,
  "(unverified)", the ~150-line cap on [AGENTS.md](../../AGENTS.md)) is in
  [docs-maintenance.md](docs-maintenance.md#agent-docs-agentsmd-docsagentsmd).
