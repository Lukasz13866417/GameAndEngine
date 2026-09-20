# Documentation maintenance handoff

## Class-by-class walkthrough · 2026-09-19

Added a separate fifth page, `walkthrough.html`, titled **Code walkthrough**.
It complements (does not replace) the component/dependency Codebase catalog.
`walkthrough.js` contains **31 named C++ abstraction cards** arranged along five
reading paths: CPU geometry, shader construction, backend draw submission,
editor authoring, and worker delivery/presentation. Each card includes purpose,
a short API example, actual source links, owns/borrows and collaboration links.
**19 excerpts** are checked against current source; remaining sketches are
explicitly labeled as incomplete usage examples. Tree nesting means reading
order, not ownership or inheritance.

Inspected actual declarations in `gfx/{record,vertex_stream,mesh}.hpp`,
`dsl/expr.hpp`, `shader/{stage,program,arguments,ir}.hpp`,
`render/{backend,renderer}.hpp`, `opengl/{backend,renderer,device,frame,commands,
gpu_mesh,instance_buffer}.hpp`, and the editor session/project/patch/update,
viewport/transport/runtime/blueprint-renderer declarations. In particular,
StageContext **borrows** the factory's FunctionBuilder; Renderer requires its
backend type, is not a runtime router, and has no default portable backend.

Navigation now has five entries, all offline. Start here remains plain, as does
the concrete Demo walkthrough. All **91 previous topic bookmarks** remain; the
browser suite checks all **128 current IDs** through legacy-style entry URLs.
Search, keyboard expansion, direct class links and cross-page related links work.
`docs/codebase.md` links the new reading path; no engine/CMake/assets were edited.
The component catalog's finalized content contains 89 sections and 10 checked
source excerpts, alongside the separate class walkthrough.

Validation: **11 content tests pass**, including source-excerpt drift, actual
CMake edges/scopes, local links and class API/lifetime coverage. JavaScript syntax
checks pass. The installed Playwright/Chromium offline suite passed with five
pages, JavaScript-disabled overview/demo, all 128 bookmarks, history,
keyboard/search, no external requests/page errors, and widths 375/768/1440.
Expanded Commands cards at desktop/mobile and the new landing page were visually
inspected. Code samples scroll horizontally within their block on narrow screens.
Screenshots: `/tmp/vng-guide-browser-jlFPbo` (temporary artifacts).

Next useful finite pass: a task-oriented extension tutorial (e.g. add a blueprint
manipulation descriptor or effect field), with a small compilable example fixture.
Current source-excerpt matching prevents drift but is not full snippet compilation.
Ownership prose still requires review when actual C++ members change; CMake link
tests cannot prove borrowing lifetimes. Keep target catalog and class walkthrough
separate, and avoid duplicating every implementation helper as another card.

## Detailed codebase API pass · 2026-09-19

`codebase-details.js` adds 85 source-linked API/lifetime sections across all 33
mapped components. Expanded cards now explain concrete types, data flow,
ownership, failure/lifetime constraints and extension boundaries, rather than
only linking elsewhere. Search includes these explanations and snippets.
`docs/codebase.md` also covers records, shader construction versus execution,
typed arguments, renderers, graphics state, providers, UI/text and rigging.

The renderer contract is explicit: `render::Renderer<Ticket, Backend>` is the
neutral base; `opengl::Renderer<Ticket>` is its OpenGL alias. The documents
distinguish compile-time backend compatibility from runtime device/context
ownership and do not claim another production graphics backend exists.
The plain Start here page and four-page organization are preserved.

The documentation agent landed the content and tests; after its interruption,
the coordinating agent finished validation:

- All 10 content tests pass, including source excerpts and actual CMake edges.
- Syntax checks pass for the detailed catalog, application and browser test.
- The offline Chromium suite passes: all 91 legacy bookmarks, cross-page
  history, keyboard/search, dependency links, no script errors or external
  requests, and 375/768/1440-pixel layouts. Start here and the demo also work
  with JavaScript disabled.
- Expanded desktop renderer and narrow runtime/dependency screenshots were
  inspected. Temporary artifacts: `/tmp/vng-guide-browser-XIETK5`.

Remaining work below is still useful, especially focused task-oriented tours
and independently compilable API examples. This pass documents current APIs;
it is not an exhaustive signature reference or a background sync service.

## Four-page newcomer pass · 2026-09-18

The current entry point is a **plain overview**, not a tree:

- `index.html`: visible editor/codebase concepts, boundaries, vocabulary and a
  small schema example. No `details` elements, hidden topic hierarchy or search
  controls to navigate before learning the basics.
- `editor.html`: the editor concept tree and edit/mesh workflows.
- `codebase.html`: component dependencies, source map, ownership tree and
  reusable engine concepts. The original 33-component inventory is retained.
- `demo.html`: a linear walkthrough of the actual `vng_file_mesh_demo`, including
  loading, renderer-owned shader/resources, ticket submission and diagnostics.

`pages.js` assigns topic IDs to pages. All 91 existing topic bookmarks continue
to work through `index.html#topic` redirects. Within-page links remain hashes;
cross-page related links use their canonical file. Search/reset/collapse apply
to the current detailed page, not the overview. Overview and demo content and
page navigation work without JavaScript; legacy redirects and topic trees need it.

The demo was checked against `examples/file_mesh.cpp`,
`file_mesh_renderer.{hpp,cpp}`, `file_mesh_types.hpp`,
`support/{options.cpp,glfw_opengl_session.hpp}`, and the real cube asset. Six
marked snippets are matched to source in tests (whitespace-insensitive); this
is drift checking, not compilation of independent snippet programs.

Validation passed: all **nine** content tests; syntax checks for data/codebase,
pages/app and browser-test scripts; and the offline Chromium browser suite.
Browser coverage includes four real pages, all 91 legacy bookmarks, cross-page
Back navigation, keyboard/search/reset, dependency/reverse links, static pages
with JavaScript disabled, and 375/768/1440-pixel layouts without horizontal
document overflow. Desktop/mobile overviews, the concrete demo, Codebase landing
page and dependency/ownership cards were visually inspected. Final screenshots:
`/tmp/vng-guide-browser-ALWRCo` (temporary artifacts, not required to view the docs).

Concurrent UI work was communicated by the parent and rechecked in current
`docs/editor.md`: row-one file/history/Logs/Settings actions, row-two view/camera/
world/region/debug controls and More.../Tools... overflow. New prose uses those
labels or general responsibility descriptions, not obsolete sidebar paths.
The scroll-moves-camera default was also checked in `settings.hpp` and corrected.

Remaining work below still applies. The project README is outside this pass;
new links should prefer the four canonical pages, although old links work.
No runtime/CMake/assets edits, package installs or C++ builds were performed.

## First codebase pass · 2026-09-18

Scope: `docs/explore/**` and `docs/codebase.md`. No engine code, scene assets,
build configuration or user preferences changed. The parent brief is
[`../documentation_work.md`](../documentation_work.md).

Implemented a first-class Codebase branch with a real-target catalog, direct
dependency disclosures and derived reverse users; a separately labeled runtime
ownership tree; source/API/test links; and backend-dispatch/edit-delivery reading
paths. Existing feature topics/bookmarks remain. The Markdown companion is a
source tour rather than an exhaustive API reference.

### Source evidence

- `CMakeLists.txt`, especially principal libraries and editor/application targets.
- `include/vng/render/{renderer,program}.hpp`: ticket identity and device-selected
  compilation; the neutral render target does not link the shader library.
- `include/vng/{dsl,shader,glsl,opengl}` and `examples/file_mesh_direct.cpp`:
  recording, emission, compilation and typed submission boundaries.
- `examples/file_mesh_renderer.hpp`: concrete resource ownership/ticket API.
- `include/vng/resources/provider.hpp`: recipes, context-aware dispatch and
  retained/type-erased provider storage.
- `examples/editor/{editing_session,project,viewport_interaction}.hpp`: actual
  state ownership, transactions and borrowed tool dependencies.
- `examples/editor/{preview_updates,viewport_session,preview_mailbox,presented_view}.hpp`
  and `include/vng/editor/preview.hpp`: ordered authoring versus replaceable
  viewing intent, process ownership and completed-image metadata.
- `examples/editor/runtime.cpp`, `blueprint_mesh_renderer.hpp` and
  `examples/editor_worker.cpp`: context-owning worker, shared program lifetime,
  blueprint resource batching and rendering state.
- `examples/editor/app.cpp` and `editor_layout.hpp`: Scene/Keyframe/Properties
  sidebar and UI-owned editable detached viewport.

### Validation

Passed:

```sh
node --test docs/explore/tests/content.test.cjs
node --check docs/explore/data.js
node --check docs/explore/codebase.js
node --check docs/explore/app.js
node --check docs/explore/tests/browser.cjs
```

All seven content tests pass, including every mapped component's complete direct
CMake links/scopes, local source links, bookmarks and selected layering boundaries.

The optional browser test passed with the already installed Playwright/Chromium:

```sh
node docs/explore/tests/browser.cjs \
  /tmp/vng-guide-tests-HHFxJ6/node_modules/playwright \
  /home/luke/.cache/ms-playwright/chromium-1228/chrome-linux64/chrome
```

It checks all 91 bookmarks from reloaded documents, keyboard expansion,
dependency/reverse links, build-order labeling, source-path search, history,
reset, no external requests/page errors, and widths 375/768/1440. Actual desktop
and narrow dependency cards and the ownership card were visually inspected.
Screenshots go into a new `/tmp/vng-guide-browser-*` directory on each run;
they are not retained repository assets or guaranteed future paths.
No packages were installed and no C++ build was run for these docs-only edits.

### Remaining useful work

1. Add focused reference pages for the most-used current public signatures and
   diagnostics, preferably excerpts verified by compile-only documentation tests.
   Current snippets are illustrative, not independently compilable programs.
2. Expand the target catalog to remaining standalone demo/generator/support
   targets if readers need those paths. Current reverse links deliberately cover
   mapped components only, not every executable or include dependency.
3. Create task-oriented mini-tours for adding a blueprint/manipulation descriptor,
   effect inspector fields and a new neutral API/backend implementation pair.
4. Keep checking runtime ownership manually when component members or app
   composition changes. CMake tests catch link drift, not undocumented lifetime
   requirements or source include coupling.
5. Cross-link the Codebase guide from the project README if desired; that file is
   outside this agent's write scope. No external hosting/publishing is required.

The guide reflects the source inspected during this finite pass; it is not an
always-running synchronization service. Resume from this note, the durable brief
and current source, rather than assuming a future agent has hidden conversation
memory.
