# Documentation work brief

This is a handoff for documentation work, not an always-running service or a
replacement for reading the current source. The first codebase pass was completed
by `codebase_docs` on 2026-09-18: 33 real components, ownership/dependency details
and the Markdown source tour. The second finite pass is also complete: four
pages, a non-tree introduction and a source-checked concrete demo walkthrough.
The third pass, completed on 2026-09-19, adds 85 source-linked API/lifetime
sections across all 33 components and expands the readable codebase tour.
It includes the explicit `render::Renderer<Ticket, Backend>` /
`opengl::Renderer<Ticket>` contract. The coordinating agent finished content
and offline browser validation after the documentation agent was interrupted.
Validation and handoff notes are recorded in `docs/explore/maintenance.md`.
The fourth finite pass is complete: a separate fifth **Code walkthrough** page
with 31 real C++ abstraction cards, 19 checked source excerpts, and owns/borrows
descriptions. All 11 content tests and the five-page offline browser suite pass.

## Current assignment

The latest finite assignment is complete: `docs/explore/walkthrough.html` walks
through actual classes, separately from the target/dependency inventory. Preserve
all five pages: plain **Start here**, **Editor**, **Codebase**, expandable **Code
walkthrough**, and plain concrete **Demo walkthrough**. Keep stable bookmarks,
offline routing, source-checked excerpts and the Markdown companion. Newcomers
should be able to answer:

- Where does this feature live, and which file should I read first?
- What does this component own, and what does it borrow or depend on?
- Which code is backend-neutral, and where does OpenGL/GLFW enter?
- How does an editor action reach the document, preview worker and displayed image?
- Where would I change or extend a particular behavior, and which tests cover it?

Reuse the existing offline HTML/JS infrastructure rather than introducing another
documentation framework. Keep ownership/containment separate from dependency
links; a tidy tree must not conceal real cross-component dependencies. Expose
details progressively, keep explanations short, and link to real source files.
Preserve bookmarks, keyboard access, search and offline operation.

## Project preferences to preserve

- Prefer intuitive components with clear owners, visible dependencies and small,
  distinct responsibilities. Tree-like ownership is an architectural preference,
  not a claim that all dependency graphs are trees.
- Public APIs should require little boilerplate. Avoid abstractions that merely
  hide another abstraction or feel like a workaround.
- Keep backend-neutral types and contracts independent of OpenGL and GLFW;
  specialized implementations depend on neutral interfaces, not the reverse.
- Make resource ownership, context lifetime and reconstruction responsibilities
  explicit. Do not confuse construction providers with resource ownership.
- Keep blueprints separate from their scene instances. Instance transforms and
  editable region boundaries must not silently mutate shared blueprints.
- Keep authored document changes separate from private viewport navigation and
  presentation state. Describe narrow updates and batching accurately.
- Explain the current improved API, not a history of superseded designs. Label
  intended architecture and deferred features as such; source code is evidence
  for what is implemented.
- Examples should make engine features understandable; setup/parsing/support
  details belong in helpers when practical.

## Initial source landmarks to verify

Start with `CMakeLists.txt`, `include/vng`, `src`, `examples/editor`,
`examples/support` and their tests. Existing guides include
`docs/editor_boundaries.md`, `docs/editing_session.md`, `docs/renderer_api.md`,
`docs/shader_vertex_pipeline.md`, `docs/resources.md` and `docs/regions.md`.
Do not infer relationships from filenames alone.

The most recent editor change moved the former left sidebar into the right
sidebar's **Scene** tab, alongside **Keyframe values** and **Instance properties**
(**Blueprint geometry** in mesh mode). The editable viewport uses the freed
width; detached controls use the same tabbed authoring panel. See
`examples/editor/app.cpp`, `examples/editor/editor_layout.hpp` and `docs/editor.md`.

The subsequent toolbar update groups Open scene/Save/Save As/Import/Undo/Redo,
Logs/Settings and playback/window actions in the first row. The second row holds
View, Camera settings, world-bounds/region controls and debug display. Narrow
windows use More... and Tools... overflow. Numerical bounds and region creation
remain in Scene; the camera flyout is available across sidebar tabs. Prefer
general responsibility descriptions over fragile screen-coordinate instructions.

## Concurrent-work boundaries

The documentation agent may edit `docs/explore/**`, `docs/codebase.md` and this brief.
Other work can continue in engine/editor files. The coordinating agent owns this
assignment. Do not change runtime code to make a diagram true; report discrepancies.
Preserve existing/untracked work. Do not commit, push, publish, install a daemon
or create an indefinite scheduled loop as part of this assignment.

Use a finite pass: inspect, improve, validate and report. Later passes can resume
from this brief plus a documentation maintenance note and current source.
Conversation context is a useful snapshot, not a live feed of later decisions.

## Validation and handoff

Run the documentation content tests and JavaScript syntax checks. Exercise the
interactive guide in a browser when available, including expansion, dependency
links, search, keyboard navigation and narrow layouts. Use existing browser-test
instructions in `docs/explore/README.md`; do not rebuild C++ for prose changes.

Leave a short maintenance note under `docs/explore` with the verified source
landmarks, checks actually run, remaining gaps and suggested next pass. Report
completion to the coordinating thread. Documentation should describe the source
snapshot inspected; call out concurrent changes that could not be rechecked.
