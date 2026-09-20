/* Page ownership of stable topic IDs. No network requests or module loader. */
(() => {
  "use strict";
  const flatten = nodes => nodes.flatMap(node => [node, ...flatten(node.children || [])]);
  const topics = new Map(flatten(window.VNG_GUIDE).map(node => [node.id, node]));
  const pages = [
    { id: "start", file: "index.html", title: "Start here", roots: [],
      staticIds: ["start", "big-picture", "editor-overview", "engine-overview", "philosophy", "vocabulary"] },
    { id: "editor", file: "editor.html", title: "Editor", roots: ["editor", "move-an-instance", "edit-a-mesh"], staticIds: [] },
    { id: "codebase", file: "codebase.html", title: "Codebase", roots: ["codebase", "engine"], staticIds: [] },
    { id: "walkthrough", file: "walkthrough.html", title: "Code walkthrough", roots: ["code-walkthrough"], staticIds: [] },
    { id: "demo", file: "demo.html", title: "Demo walkthrough", roots: [],
      staticIds: ["follow-the-data", "draw-a-frame", "demo-load", "demo-resources", "demo-shaders", "demo-submit", "diagnostics", "demo-try"] }
  ];
  const routes = new Map();
  for (const page of pages) {
    for (const id of page.staticIds) routes.set(id, page);
    for (const root of page.roots) {
      for (const node of flatten([topics.get(root)])) routes.set(node.id, page);
    }
  }
  window.VNG_GUIDE_PAGES = pages;
  window.VNG_TOPIC_ROUTES = routes;
})();
