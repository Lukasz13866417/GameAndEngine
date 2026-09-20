const { test } = require("node:test");
const assert = require("node:assert/strict");
const { readFileSync, existsSync } = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "..");
const repository = path.resolve(root, "../..");
const context = { window: {} };
vm.runInNewContext(readFileSync(path.join(root, "data.js"), "utf8"), context);
vm.runInNewContext(readFileSync(path.join(root, "codebase.js"), "utf8"), context);
vm.runInNewContext(readFileSync(path.join(root, "codebase-details.js"), "utf8"), context);
vm.runInNewContext(readFileSync(path.join(root, "walkthrough.js"), "utf8"), context);
vm.runInNewContext(readFileSync(path.join(root, "pages.js"), "utf8"), context);
const roots = context.window.VNG_GUIDE;
const flatten = nodes => nodes.flatMap(node => [node, ...flatten(node.children || [])]);
const nodes = flatten(roots);
const ids = new Set(nodes.map(node => node.id));
const pages = context.window.VNG_GUIDE_PAGES;
const routes = context.window.VNG_TOPIC_ROUTES;

test("every topic has a stable unique ID and beginner-level introduction", () => {
  assert.equal(roots.length, 6);
  assert.equal(ids.size, nodes.length);
  assert.ok(nodes.length >= 30);
  for (const node of nodes) {
    assert.match(node.id, /^[a-z][a-z0-9-]*$/);
    assert.ok(node.title && node.summary && node.kind, node.id);
    assert.ok(node.description?.length || node.steps?.length, node.id);
    assert.ok(node.summary.length < 170, `Keep ${node.id}'s summary short`);
    assert.ok(!node.children || node.children.length > 0, node.id);
  }
});

const components = nodes.filter(node => node.component);
const byTarget = new Map(components.map(node => [node.component.target, node]));
test("every component has concrete source-backed API and lifetime guidance", () => {
  let excerptCount = 0;
  const compact = text => text.replace(/\s+/g, "");
  for (const node of components) {
    assert.ok(node.sections?.length >= 2, `${node.id}: replace stubs with substantive sections`);
    const text = node.sections.flatMap(s => s.paragraphs).join(" ");
    assert.ok(text.length >= 450, `${node.id}: explain actual behavior, not just its name`);
    for (const section of node.sections) {
      assert.ok(section.title && section.paragraphs.length && section.source, node.id);
      const source = path.resolve(repository, section.source);
      assert.ok(source.startsWith(repository + path.sep) && existsSync(source), `${node.id}: ${section.source}`);
      if (section.exact) {
        excerptCount++;
        assert.ok(section.code, node.id);
        assert.ok(compact(readFileSync(source, "utf8")).includes(compact(section.code)),
          `Outdated codebase excerpt: ${node.id} / ${section.source}`);
      }
    }
  }
  assert.ok(excerptCount >= 6);
  const renderer = JSON.stringify(byTarget.get("vng_render"));
  assert.match(renderer, /Renderer<Ticket, Backend>/);
  assert.match(renderer, /RendererFor/);
  assert.match(renderer, /two OpenGL devices/);
});
const cmake = readFileSync(path.join(repository, "CMakeLists.txt"), "utf8").replace(/#[^\n]*/g, "");

test("component dependencies match actual direct CMake links, not invented ownership", () => {
  assert.ok(components.length >= 30);
  assert.equal(byTarget.size, components.length);
  const declared = new Map();
  for (const [, body] of cmake.matchAll(/target_link_libraries\s*\(([^)]*)\)/g)) {
    const [target, ...tokens] = body.trim().split(/\s+/);
    if (!declared.has(target)) declared.set(target, []);
    let visibility;
    for (const token of tokens) {
      if (["PUBLIC", "PRIVATE", "INTERFACE"].includes(token)) visibility = token;
      else declared.get(target).push({ target: token, visibility });
    }
  }
  for (const node of components) {
    const component = node.component;
    assert.match(cmake, new RegExp(`add_(?:library|executable)\\(${component.target}\\s`));
    const links = declared.get(component.target) || [];
    const mapped = [...component.dependencies, ...component.external.map(item => ({ target: item.name, visibility: item.visibility }))];
    assert.deepEqual([...new Set(mapped.map(item => item.target))].sort(),
      [...new Set(links.map(item => item.target))].sort(), `${component.target}: document every direct link`);
    for (const edge of mapped) {
      assert.ok(links.some(link => link.target === edge.target && link.visibility === edge.visibility),
        `${component.target}: ${edge.visibility} ${edge.target} is not declared`);
    }
    for (const edge of component.dependencies) {
      assert.ok(byTarget.has(edge.target), `${component.target}: missing component ${edge.target}`);
      assert.notEqual(edge.target, component.target);
    }
    for (const edge of component.external) assert.ok(!byTarget.has(edge.name), `Mapped target ${edge.name} must be a navigable dependency`);
    for (const target of component.buildAfter) {
      assert.ok(byTarget.has(target));
      assert.match(cmake, new RegExp(`add_dependencies\\(${component.target}\\s+${target}\\)`));
      assert.ok(!component.dependencies.some(edge => edge.target === target), "Ordering must not masquerade as linking");
    }
  }
});

test("core architectural boundaries and explicit ownership stay inspectable", () => {
  const dependencies = target => byTarget.get(target).component.dependencies.map(edge => edge.target);
  assert.ok(!dependencies("vng_opengl").includes("vng_window_glfw"));
  assert.ok(dependencies("vng_glfw_opengl").includes("vng_window_glfw"));
  assert.ok(dependencies("vng_rig_opengl").includes("vng_rig"));
  assert.ok(!dependencies("vng_rig").includes("vng_rig_opengl"));
  assert.deepEqual(Array.from(dependencies("vng_render")), ["vng_gfx"]);
  const session = nodes.find(node => node.id === "code-session-owner");
  assert.ok(session.ownership.owns.some(item => item.includes("Document, ViewportState")));
  const runtime = nodes.find(node => node.id === "code-runtime-owner");
  assert.ok(runtime.ownership.borrows.some(item => item.includes("shared Program")));
  for (const node of nodes.filter(node => node.ownership)) {
    assert.ok(node.ownership.owns.length);
    assert.ok(Array.isArray(node.ownership.borrows));
    assert.ok(node.links?.length);
  }
  assert.match(JSON.stringify(nodes.find(node => node.id === "code-edit-delivery")), /send_latest/);
});

test("the readable codebase tour links only to existing local material", () => {
  const file = path.join(repository, "docs/codebase.md");
  const text = readFileSync(file, "utf8").replace(/```[^\n]*\n[\s\S]*?```/g, "");
  for (const [, href] of text.matchAll(/\]\(([^)]+)\)/g)) {
    const [name, bookmark] = href.split("#");
    assert.ok(existsSync(path.resolve(path.dirname(file), name)), href);
    if (name.startsWith("explore/") && name.endsWith(".html") && bookmark) {
      assert.equal(routes.get(bookmark)?.file, path.basename(name), href);
    }
  }
});

test("every related topic and local documentation/source link exists", () => {
  for (const node of nodes) {
    for (const id of node.related || []) {
      assert.ok(ids.has(id), `${node.id} refers to missing ${id}`);
      assert.notEqual(id, node.id);
    }
    for (const link of node.links || []) {
      assert.ok(link.label);
      assert.ok(!/^[a-z]+:/i.test(link.href), "Keep references local");
      const target = path.resolve(root, link.href.split("#")[0]);
      assert.ok(target.startsWith(repository + path.sep), target);
      assert.ok(existsSync(target), `${node.id}: ${link.href}`);
    }
  }
});

test("all page assets, local links and stable topic routes are available offline", () => {
  assert.deepEqual(Array.from(pages, page => page.id), ["start", "editor", "codebase", "walkthrough", "demo"]);
  for (const id of ids) assert.ok(routes.has(id), `Preserve old bookmark ${id}`);
  for (const page of pages) {
    const html = readFileSync(path.join(root, page.file), "utf8");
    const localIds = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map(match => match[1]));
    for (const id of page.staticIds) assert.ok(localIds.has(id), `${page.file}: static ${id}`);
    for (const [, href] of html.matchAll(/(?:src|href)="([^"]+)"/g)) {
      assert.ok(!/^[a-z]+:/i.test(href), `Keep references local: ${href}`);
      const [file, id] = href.split("#");
      if (!file) {
        assert.ok(localIds.has(id) || routes.get(id)?.id === page.id, `${page.file}: ${href}`);
        continue;
      }
      assert.ok(existsSync(path.resolve(root, file)), `${page.file}: ${href}`);
      if (id && pages.some(candidate => candidate.file === file)) {
        assert.equal(routes.get(id)?.file, file, `Use canonical page link: ${href}`);
      }
    }
    assert.match(html, /<nav id="navigation"/);
    if (page.roots.length) {
      assert.match(html, /<noscript>/);
      assert.match(html, /aria-live="polite"/);
    }
  }
  const js = readFileSync(path.join(root, "app.js"), "utf8");
  assert.doesNotMatch(js, /\b(?:fetch|XMLHttpRequest)\s*\(/);
  assert.doesNotMatch(js, /\.innerHTML\s*=/);
});

test("Start here is a plain overview, not a hidden or collapsed tree", () => {
  const html = readFileSync(path.join(root, "index.html"), "utf8");
  assert.doesNotMatch(html, /<details|id="tree"|class="explorer"/);
  assert.equal(pages.find(page => page.id === "start").roots.length, 0);
  assert.match(html, /The editor, at a glance/);
  assert.match(html, /The codebase, in broad pieces/);
  assert.equal((html.match(/<article>/g) || []).length, 12);
  assert.match(html, /schema\.map/);
});

test("the separate code walkthrough follows real named abstractions with APIs and lifetimes", () => {
  const page = pages.find(page => page.id === "walkthrough");
  assert.deepEqual(Array.from(page.roots), ["code-walkthrough"]);
  const rootNode = nodes.find(node => node.id === "code-walkthrough");
  assert.match(rootNode.description.join(" "), /reading sequence, not inheritance or ownership/);
  const symbols = flatten([rootNode]).filter(node => node.symbol);
  assert.ok(symbols.length >= 30);
  const compact = text => text.replace(/\s+/g, "");
  let exact = 0;
  for (const node of symbols) {
    assert.equal(routes.get(node.id).file, "walkthrough.html");
    assert.ok(!node.component, "Class identity is not a CMake target");
    assert.ok(node.ownership.owns.length && node.ownership.borrows.length, node.id);
    assert.ok(node.sections.some(section => section.code), node.id);
    for (const section of node.sections) {
      const source = path.resolve(repository, section.source);
      assert.ok(existsSync(source), `${node.id}: ${section.source}`);
      if (section.exact) {
        exact++;
        assert.ok(compact(readFileSync(source, "utf8")).includes(compact(section.code)),
          `Outdated walkthrough excerpt: ${node.id} / ${section.source}`);
      }
    }
  }
  assert.ok(exact >= 15);
  for (const id of ["walk-record", "walk-expr", "walk-renderer", "walk-commands", "walk-session",
    "walk-viewport-state", "walk-patch", "walk-updates", "walk-runtime", "walk-blueprint-renderer"]) {
    assert.ok(symbols.some(node => node.id === id), id);
  }
});

test("the concrete demo's marked code excerpts match actual source", () => {
  const html = readFileSync(path.join(root, "demo.html"), "utf8");
  assert.doesNotMatch(html, /<details|id="tree"/);
  assert.match(html, /vng_file_mesh_demo --analyze/);
  const decode = text => text.replaceAll("&lt;", "<").replaceAll("&gt;", ">").replaceAll("&amp;", "&").replaceAll("&quot;", '"');
  const compact = text => text.replace(/\s+/g, "");
  const excerpts = [...html.matchAll(/<pre data-source="([^"]+)"><code>([\s\S]*?)<\/code><\/pre>/g)];
  assert.ok(excerpts.length >= 6);
  for (const [, source, snippet] of excerpts) {
    const implementation = readFileSync(path.resolve(root, source), "utf8");
    assert.ok(compact(implementation).includes(compact(decode(snippet))), `Outdated demo excerpt from ${source}`);
  }
});

test("important architecture distinctions remain visible in the content", () => {
  const byId = id => JSON.stringify(nodes.find(node => node.id === id));
  assert.match(byId("shader-dsl"), /intermediate representation/);
  assert.match(byId("graphics-state"), /frame owns output encoding/);
  assert.match(byId("viewport"), /does not create document revisions/);
  assert.match(byId("mesh-drafts"), /does not secretly publish/);
  assert.match(byId("diagnostics"), /single-mesh\/single-instance/);
});
