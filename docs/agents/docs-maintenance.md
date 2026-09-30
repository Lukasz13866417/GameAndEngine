# Keeping docs in sync

Read this when you change code that the docs quote or describe. Also read it when you edit `docs/`, `docs/explore/`,
`README.md`, `examples/README.md`, `AGENTS.md` or `docs/agents/`, or when `vng_guide_tests` fails.

**TL;DR**
- The only automated docs check is `vng_guide_tests`: `node --test docs/explore/tests/content.test.cjs`, about 0.2 s,
  no build needed. It checks the offline guide in `docs/explore/`, the links in `docs/codebase.md`, and that
  `docs/editor_components.md` names each editor role class. Nothing else.
- It fails when you edit code the guide quotes verbatim, change `target_link_libraries` or `add_dependencies` of a
  guide-mapped target, rename or move an editor role class the test lists (such as `EditingWorkspaceUI` in
  `examples/editor/workspace_ui.hpp`), or move or rename a file the guide or `docs/codebase.md` links to.
- No test checks the other docs (the rest of `docs/*.md`, both READMEs, `AGENTS.md`, `docs/agents/`). Grep for what you
  changed and fix it in the same PR.
- Volatile facts go only in [status.md](status.md). Never change runtime code to make a doc true, and never describe
  planned work as implemented.

## 1. Where each kind of fact lives

| Kind of fact | Home | Notes |
|---|---|---|
| Rules every agent needs in every session | [AGENTS.md](../../AGENTS.md) | Loaded every session. Codex reads it directly; Claude Code reads it through `CLAUDE.md` (`@AGENTS.md`), also from parent directories of a worktree. Keep it at about 150 lines, durable and version-agnostic |
| Agent how-tos by area | `docs/agents/*.md` ([index](README.md)) | Commands, recipes, gotchas. They link to topic docs instead of restating them |
| Dated, volatile facts (branches, open PRs, failing tests, who edits what, current sizes) | [status.md](status.md) | Each fact carries a date and the command that re-checks it. Other files link here and never repeat these facts |
| How a subsystem, demo or shot works | `docs/<topic>.md` | One per subsystem or cinematic, written for people as well as agents |
| Source tour without JavaScript | [codebase.md](../codebase.md) | Its links are test-checked |
| Interactive offline guide | [explore/](../explore/README.md) | Drift-tested: quoted code, CMake links, local links, routes |
| Log of documentation passes | [explore/maintenance.md](../explore/maintenance.md) | Add a dated `## <pass> · YYYY-MM-DD` section per guide pass |
| Owner's standing preferences | [collaboration.md §6](collaboration.md#6-the-owners-preferences-in-full) | Restates [documentation_work.md](../documentation_work.md) ("Project preferences to preserve") and the owner's saved memory. AGENTS.md section 1 summarizes it |

## 2. What `vng_guide_tests` enforces

```sh
node --test docs/explore/tests/content.test.cjs    # vng_guide_tests, ~0.2 s, no build
node --test tests/layering/layering.test.cjs        # vng_layering_tests, include and link layering (build-and-test.md)
ctest --test-dir build -R '^vng_(guide|layering)_tests$' --output-on-failure   # the same two through CTest
```
- The tests have no CTest label, so select them with `-R`. CTest registers them only if `find_program` found `node`
  at configure time; CMake checks no version, but `node --test` needs Node 18 or newer. Without node, a full ctest
  silently has two fewer tests.
- If the test already fails before your change, that is someone else's drift: see [status.md](status.md). Fix it only if
  your task owns that code. Otherwise say in your PR that it was failing already.

Each of the 12 tests in `docs/explore/tests/content.test.cjs`, and the code change that most often breaks it:

| Test (start of its name) | Enforces | Broken by a code change when you... |
|---|---|---|
| every topic has a stable unique ID | 6 root topics; ≥ 30 topics; ids `^[a-z][a-z0-9-]*$`; summaries < 170 chars | (guide edits only) |
| every component has concrete source-backed API | each mapped target has ≥ 2 sections totalling ≥ 450 chars; every section `source` exists; `exact` excerpts occur in the source; ≥ 6 exact component excerpts; required `vng_render` phrases | edit quoted code, or move or delete a file named in any section's `source` |
| component dependencies match actual direct CMake links | each mapped target's documented links equal its `target_link_libraries` (the same set, and each visibility declared); every `buildAfter` entry has a matching `add_dependencies(<target> <dep>)` | change links, visibility or `add_dependencies` of a mapped target, or rename its `add_library` |
| core architectural boundaries | `vng_opengl` does not link `vng_window_glfw`, and `vng_glfw_opengl` does; `vng_render` links exactly `vng_gfx`, `vng_resources`; `vng_rig_opengl` → `vng_rig`, never the reverse; named ownership phrases | change one of those links. That is an architecture change: ask the owner first |
| editor role names match real components | for each of the 17 `examples/editor/` headers in the test's `types` map (e.g. `workspace_ui.hpp`, `camera_pointer_logic.hpp`, `scene_move_gizmo.hpp`; not every `*_gizmo.hpp` is listed), the header declares `class <Name>` and `docs/editor_components.md` mentions `<Name>`; that doc keeps two fixed phrases | rename, move or delete one of those classes or headers. Update the test's map and `docs/editor_components.md` together |
| the readable codebase tour links only to existing local material | every `](...)` in `docs/codebase.md` outside code fences exists; `explore/*.html#id` anchors route to that page | move or rename a linked file |
| every related topic and local documentation/source link exists | `related` ids exist; every `links[].href` is local and exists | move or rename a linked file |
| all page assets, local links and stable topic routes | exactly 5 pages in order `start, editor, codebase, walkthrough, demo`; every id routed; HTML `src`/`href` local and existing; no `fetch`, `XMLHttpRequest` or `.innerHTML =` in `app.js` | move or rename a file an HTML page links to (for example `examples/file_mesh*.cpp`, `examples/assets/colored_cube.vmesh`, `docs/editor.md`, `README.md`) |
| Start here is a plain overview | `index.html`: no `<details>`, exactly 12 `<article>`, fixed headings | (guide edits only) |
| the separate code walkthrough | ≥ 30 class cards; each has owns and borrows; ≥ 15 exact excerpts that occur in the source; 10 required card ids | edit quoted code, or move or delete a file a card's `source` names |
| the concrete demo's marked code excerpts | every `<pre data-source="...">` excerpt in `demo.html` occurs in its source (≥ 6) | edit `examples/file_mesh*.{cpp,hpp}` |
| important architecture distinctions | required phrases in 5 topics | (guide edits only) |

## 3. Quoted code (drift-checked excerpts)

- **Where the excerpts live:**
  - `docs/explore/codebase-details.js`: `section(title, paragraphs, source, code, exact)`. Only `exact = true` is
    checked; the rest are sketches.
  - `docs/explore/walkthrough.js`: `type(id, symbol, ..., source, code, owns, borrows, related, exact)`. The last
    argument `true` marks a checked excerpt.
  - `docs/explore/demo.html`: `<pre data-source="../../path"><code>...</code></pre>`, HTML-escaped.
- **How they are compared:** all whitespace is removed from both the excerpt and the source file, and the excerpt must
  then be a substring of the file. Reindenting and rewrapping are safe. Renaming, reordering or retyping any token
  breaks it.
- **List the quoted files** (run from the repo root; the test itself is the authority):
```sh
node -e 'const fs=require("fs"),vm=require("vm"),c={window:{}};
for(const f of ["data","codebase","codebase-details","walkthrough","pages"])vm.runInNewContext(fs.readFileSync(`docs/explore/${f}.js`,"utf8"),c);
const all=n=>n.flatMap(x=>[x,...all(x.children||[])]);
const s=all(c.window.VNG_GUIDE).flatMap(n=>(n.sections||[]).filter(x=>x.exact).map(x=>x.source));
for(const[,f]of fs.readFileSync("docs/explore/demo.html","utf8").matchAll(/<pre data-source="\.\.\/\.\.\/([^"]+)"/g))s.push(f);
console.log([...new Set(s)].sort().join("\n"))'
```
  These are the quoted files when this guide was written (no `examples/editor/workspace*` file is quoted):
  - `examples/editor/`: `blueprint_mesh_renderer.hpp`, `document_patch.hpp`, `project.hpp`, `runtime.hpp`,
    `viewport_session.hpp`;
  - `examples/`: `file_mesh.cpp`, `file_mesh_direct.cpp`, `file_mesh_renderer.cpp`, `file_mesh_types.hpp`, `glow.cpp`,
    `rigging.cpp`, `support/earth_clouds.hpp`;
  - `include/vng/`: `editor/preview.hpp`, `glsl/emitter.hpp`, `opengl/backend.hpp`, `opengl/renderer.hpp`,
    `shader/arguments.hpp`, `shader/ir.hpp`, `shader/stage.hpp`, `window/presentation.hpp`.

### Recipe: fix `Outdated ... excerpt`
1. Read the message. It names the topic id and the source: `Outdated codebase excerpt: <id> / <source>`,
   `Outdated walkthrough excerpt: <id> / <source>`, or `Outdated demo excerpt from <source>`.
2. Find the excerpt:
   `grep -n '<source>' docs/explore/codebase-details.js docs/explore/walkthrough.js docs/explore/demo.html`.
3. Replace the `code` text with the current source text. If the code moved to another file, change `source` too. If
   the meaning changed, fix the prose around it. In `demo.html`, escape `<`, `>`, `&` and `"`.
4. Set `exact` to false only when no current code expresses the idea. The floors must still hold: ≥ 6 exact component
   excerpts, ≥ 15 walkthrough excerpts and ≥ 6 demo excerpts.
5. Re-run the test, plus `node --check` on each edited `.js` file.

## 4. CMake links of guide-mapped targets

- The guide maps every CMake library, including scene authoring, review documents and example support.
  List them with `grep -o 'component("vng_[a-z0-9_]*"' docs/explore/codebase.js`. Non-test executables
  either have a component card or an entry in `programs`. `architecture.test.cjs` checks completeness,
  direct links and reverse Used by links against CMake; include it in documentation checks.
- When you change `target_link_libraries` of a mapped target, update its `component(target, title, summary,
  description, files, dependencies, options)` entry in `docs/explore/codebase.js`:
  - `dependencies`: the mapped targets it links. They share `options.visibility`, which defaults to `PUBLIC`. A
    dependency with a different visibility is pushed afterwards; the `vng_sun_support` line after the list shows how.
  - `options.external`: links that are not mapped targets, such as `vng_glad_dependency` or `PNG::PNG`, each with its
    visibility (`external(...)` means `PRIVATE`).
  - `options.buildAfter`: `add_dependencies` targets. Never list those as dependencies.
- The test parses CMake with regexes. `#` comments are stripped, and `target_link_libraries\s*\(([^)]*)\)` stops at the
  first `)`. A generator expression containing `)` in a mapped target's link list breaks the parse. Links inside
  `if()` branches are merged. `add_library(<target>` (or `add_executable(<target>`) must be followed by whitespace.
- For a new engine module (a directory under `include/vng` and `src`), `tests/layering/layering.test.cjs`
  (`moduleTargets`) requires a mapping. The architecture test also requires the library in the guide: a
  `component(...)` entry in `codebase.js` and a `describe("vng_<dir>", [...])` in `codebase-details.js`. Once the entry
  exists, the test requires ≥ 2 sections totalling ≥ 450 characters. Then mention the module in `docs/codebase.md`.

## 5. Links

- **Checked:** `docs/codebase.md`, and the guide's data and pages in `docs/explore/`: topic `links`, section `source`,
  and HTML `src`/`href`. Links must be local; a `scheme:` URL fails the test.
- **Unchecked:** everything else, including the Markdown in `docs/explore/` (`README.md`, `maintenance.md`). Check the
  Markdown you touched with:
```sh
node -e 'const fs=require("fs"),p=require("path");let bad=0;
for(const f of process.argv.slice(1)){const t=fs.readFileSync(f,"utf8").replace(/```[\s\S]*?```/g,"").replace(/`[^`\n]*`/g,"");
for(const[,h]of t.matchAll(/\]\(([^)\s]+)\)/g)){if(/^[a-z][a-z0-9+.-]*:/i.test(h)||h.startsWith("#"))continue;
if(!fs.existsSync(p.resolve(p.dirname(f),decodeURI(h.split("#")[0])))){console.log(`${f}: ${h}`);bad++}}}process.exit(bad?1:0)' \
  AGENTS.md README.md examples/README.md docs/*.md docs/agents/*.md docs/explore/*.md
```
  It checks that link targets exist, not heading anchors.
- **Moving or renaming a file:** `grep -rn 'old/path' docs README.md examples/README.md AGENTS.md CLAUDE.md`, then fix
  every hit.
- **Linking rules:** use relative links from the linking file's own directory. From `docs/agents/`, topic docs are
  `../<topic>.md` and the root files are `../../AGENTS.md`. Link to an existing explanation instead of copying it.

## 6. After a code change: which prose to check

Nothing fails when these go stale. Grep first:
`grep -rn 'name_or_flag' docs README.md examples/README.md AGENTS.md`.

| You changed | Check |
|---|---|
| A command-line flag, tool usage or demo option | `README.md`, `examples/README.md`, the feature's `docs/<topic>.md`, and any `docs/agents/*.md` that shows the command |
| A limit or constant | [limits-and-non-features.md](limits-and-non-features.md), plus the topic doc (for example `docs/editor.md`, `docs/timeline.md`, `docs/earth.md`) |
| A regenerated asset | the shot's doc (for example the "Budgets" paragraph in `docs/tunnel.md`), and [status.md](status.md) if it lists sizes. Prefer the measuring command over a number |
| A new demo, tool or cinematic | a new `docs/<topic>.md`, a paragraph with run commands in `README.md`, and a bullet in `examples/README.md` ([scenes-and-cinematics.md §4](scenes-and-cinematics.md#4-recipe-add-a-new-scene-generator) step 8 covers a generator without a demo). A newly committed `.vscene` or `vng_make_*` tool also gets its row in [assets.md](assets.md) §1 and §3 |
| A new engine module or library | `docs/codebase.md`, the guide (section 4), and the `README.md` feature list if users see it |
| Editor controls, shortcuts or panels | `docs/editor.md` and [editor.md](editor.md) |
| An editor component's name, role or owner | `docs/editor_components.md` (its class names are test-checked, section 2) and [editor.md](editor.md) |
| Build or test commands, targets, labels or durations | [build-and-test.md](build-and-test.md) and the Build and Test sections of `AGENTS.md` |
| Branch, PR or known-failure state | [status.md](status.md) only |
| Behaviour an agent guide describes | that guide, in the same PR |

## 7. House style

### Topic docs (`docs/*.md`)
- Start with `# Title`. The first paragraph says what it is **and what it is not**, for example "`gfx::Camera` owns
  only backend-neutral pose and lens state. It has no window, OpenGL handle, ...".
- Put exact commands in `sh` fences, runnable from the repo root as `./build/<binary>`. Tag every fence: `cpp`, `sh`,
  `text` (`glsl` where needed).
- Wrap prose near 80 columns. Name real files and symbols.
- Describe the current API, not its history. Label planned or deferred behaviour as such.
- Link a new doc from `README.md` and `examples/README.md` (see section 6).

### Agent docs (`AGENTS.md`, `docs/agents/*.md`)
- Open with "Read this when ..." and a TL;DR of 3-6 lines. Use short sections, tables and code blocks.
- Write durable, imperative rules. Put anything that will rot in [status.md](status.md), with a date and a re-check
  command.
- Make every command exact and copy-pasteable from the repo root, with its duration. Anything over 2 minutes needs the
  tool-timeout note (an explicit timeout of up to 10 min, or a background run).
- Cite paths and symbol names, not `:line` numbers, which drift.
- Link to `docs/*.md` rather than restating it.
- Mark anything you could not verify "(unverified)". Correct beats complete.
- The owner is Lukasz (GitHub `Lukasz13866417`): call them "the owner" and use they/them. Call the scratch and
  deliverables folder `$S` ([collaboration.md](collaboration.md#create-your-worktree)); never send work to `/tmp`.

### The guide (`docs/explore/`)
- Use plain classic scripts that work from `file://`. No modules, `fetch`, CDNs, web fonts or new frameworks.
- Keep every existing topic id, because ids are bookmarks. A new topic under an existing root is routed automatically.
  The root count is fixed at 6.
- Content is inserted as text, never HTML. The node schema is in [explore/README.md](../explore/README.md).
- Log the pass in `docs/explore/maintenance.md`: what you checked, which checks you ran, and what gaps remain.
- An optional browser smoke test is `node docs/explore/tests/browser.cjs /absolute/path/to/playwright`. Playwright is
  not a repo dependency; installing it needs the owner's approval.

## 8. Checklists

**Docs-only change:**
1. Run `node --test docs/explore/tests/content.test.cjs tests/layering/layering.test.cjs` (under 1 s).
2. Run `node --check` on each edited `.js` file, and the link checker (section 5) on each edited `.md` file.
3. Do not rebuild C++ for prose.

**Code change:**
1. Run `node --test docs/explore/tests/content.test.cjs` (0.2 s) and fix what it reports (sections 3-4).
2. Grep the docs for each symbol, flag and path you renamed or changed (section 6).
3. If you moved files, run the link checker (section 5).
4. Update any agent guide whose statements your change falsified. Put new volatile facts in [status.md](status.md).
