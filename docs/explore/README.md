# Engine and editor field guide

Open [`index.html`](index.html) directly in a browser. No installation, server,
network connection, or C++ build is needed. Keep the adjacent files together.
The guide also works when this repository is served by a static HTTP server.
GitHub's source-file view does not execute HTML; open a local checkout to use it.

Start with a **plain overview**, not a tree. Choose a separate page when you need
more detail:

1. [Start here](index.html): main editor and engine ideas, visible without opening anything.
2. [Editor](editor.html): scenes, editing, keyframes, tools and preview behavior.
3. [Codebase](codebase.html): components, responsibilities, ownership and dependencies.
4. [Code walkthrough](walkthrough.html): real C++ classes, their APIs and lifetimes, along a draw/edit path.
5. [Demo walkthrough](demo.html): the actual file-backed cube demo, from load to diagnostics.

The Editor and Codebase trees are **learning maps**. Component cards name real CMake
targets and expose expandable **Depends on** and **Used by** lists; the separate
ownership tree describes runtime lifetimes. This is not a generated C++ include
graph. Explicit ownership statements must match the code. API snippets are deliberately small
and point to complete examples; they omit surrounding setup and error handling.
Linked Markdown/C++ files may appear as plain text or downloads depending on
the browser/server. The HTML explanations are self-contained for newcomers.
The separate Code walkthrough uses the same expandable blocks but follows actual
named C++ types, not CMake targets. Its nesting is reading order, **not ownership**;
each card explicitly lists its own/borrowed data and includes an API excerpt.

## Using it

- On Editor/Codebase/Code walkthrough, click a block, or focus it with Tab and press Enter/Space,
  to expand it. Start here and the Demo walkthrough are ordinary readable pages;
  their content also works with JavaScript disabled.
- Search looks through the current page's topic titles, explanations, examples, and source paths.
  Matching branches open automatically; clearing search restores the preceding
  expansion state. `/` focuses search, and Escape clears it while focused.
- Topic links open ancestors and work as bookmarks, for example
  `editor.html#editing-session`. Browser Back/Forward works across pages.
  Old `index.html#editing-session` links redirect to the new location when
  JavaScript is enabled; other existing topic bookmarks are preserved too.
- “Collapse all” closes the current tree. “Reset this page” returns to its root.
- Expanding a component reveals its concrete API, ownership/lifetime notes and
  source-linked explanations. Dependencies and additional source lists have
  their own disclosures; callers need not open them just to learn the API.
- Open a Codebase component's **Dependencies & users** disclosure to follow
  direct build links. Reverse links are derived from the same data. CMake scopes
  (`PUBLIC`, `PRIVATE`, `INTERFACE`) remain visible; ordering-only build
  requirements are labeled separately.
- Search accepts source paths, CMake target names and ownership descriptions
  as well as ordinary topics. Try `vng_rig`, `EditingSession` or `commands.hpp`.
- The [readable codebase tour](../codebase.md) offers the same main paths without
  JavaScript. [maintenance.md](maintenance.md) records this pass and next gaps.

## Maintaining it

`index.html` holds the plain overview and `demo.html` the concrete walkthrough.
Their source-marked code excerpts are checked against the actual C++ files.
`data.js` holds feature/learning content; `codebase.js` adds the verified source
and component catalog. `codebase-details.js` augments each target with detailed,
source-backed API and lifetime sections. `walkthrough.js` holds the separate
class-by-class reading path and its source-checked excerpts.
`pages.js` assigns stable topic IDs to canonical pages;
some old introductory/walkthrough IDs now refer to static sections rather than
tree nodes. `app.js` renders the Editor/Codebase trees with native HTML
`details`/`summary` disclosures. `style.css` owns presentation. These are ordinary
classic scripts so they also work from `file://`, without fetching data or modules.
There are no external fonts, CDNs, analytics, or build dependencies.

Each node has:

```js
{
  id: "stable-bookmark",         // unique; keep existing IDs stable
  title: "Plain-language title",
  kind: "Engine library",       // Orientation, Concept, Backend, Editor app, Walkthrough
  symbol: "vng::opengl::Frame", // optional: class walkthrough identity, not a target
  summary: "One sentence visible while collapsed.",
  description: ["One or two brief explanations."], // optional
  boundary: "A distinction that prevents a likely misunderstanding.", // optional
  api: "Small illustrative C++ excerpt", // optional
  links: [{ label: "Source", href: "../../include/vng/gfx/mesh.hpp" }],
  related: ["another-topic-id"], // optional; collaboration is a link, not fake ownership
  children: []                  // optional
}
```

Walkthroughs can supply `steps`, an array of short strings rendered as a numbered
list. Content is inserted as text, not executable HTML. Add concepts only when
they help a newcomer understand a boundary; keep exhaustive API details in the
linked Markdown guides. Do not describe planned features as implemented ones.

Codebase nodes can additionally supply:

```js
sections: [{
  title: "A concrete API or lifetime question",
  paragraphs: ["Explain the actual types, flow, constraints and failure behavior."],
  source: "include/vng/render/renderer.hpp", // repository-relative, required
  code: "optional C++ excerpt",             // text, never HTML
  exact: false // true: whitespace-normalized excerpt must occur in source
}],
component: {
  target: "vng_rig_opengl", type: "Static library",
  dependencies: [{ target: "vng_rig", visibility: "PUBLIC" }],
  external: [{ name: "vng_glad_dependency", visibility: "PRIVATE" }],
  buildAfter: [] // add_dependencies, never confused with linked dependencies
},
ownership: {
  owns: ["Actual stored objects/data"],
  borrows: ["Explicit lifetime-bound collaborators"]
}
```

These fields are optional and independent: a target does not imply an object
owner. Document **all direct links** for every mapped target, including private
package/adapter links. Map project dependencies to component IDs rather than
calling them external packages. The content tests compare these declarations
with `CMakeLists.txt`; conditional alternatives such as GLSL's empty-source
fallback are not an inventory of a particular configured build. Used-by lists
cover mapped targets only. Check owner members and composition sites manually;
passing CMake checks does not prove ownership or include-layer correctness.

## Checks

Run the dependency-free content checks with Node.js 18 or newer:

```sh
node --test docs/explore/tests/content.test.cjs
node --check docs/explore/codebase.js
node --check docs/explore/codebase-details.js
node --check docs/explore/walkthrough.js
node --check docs/explore/pages.js
node --check docs/explore/app.js
```

These check IDs, links, cross-references, required content, offline assets,
direct CMake dependencies, page routes, non-tree overview structure,
source-matched walkthrough snippets and selected boundary regressions.
They are documentation-only checks and do not add a Node requirement to CMake.
For browser smoke testing, check expansion by keyboard, search/clear/no results,
deep links and Back/Forward, collapse/reset, and narrow-screen overflow.

An optional automated smoke test uses a separately installed Playwright package
and its Chromium browser (neither is required to view the guide):

```sh
node docs/explore/tests/browser.cjs /absolute/path/to/playwright
# Optionally append an existing Chromium executable path as the second argument.
```

It opens every old topic bookmark and checks canonical redirects, cross-page
links/history, overview/demo with JavaScript disabled, and interactions at
desktop/tablet/mobile widths. It rejects page errors and external requests and saves screenshots to a
new temporary directory. It never opens a personal browser profile.
