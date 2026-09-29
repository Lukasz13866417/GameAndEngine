const { test } = require("node:test");
const assert = require("node:assert/strict");
const { readFileSync, existsSync } = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "..");
const repository = path.resolve(root, "../..");
const context = { window: {} };
for (const file of ["data.js", "codebase.js", "architecture-data.js", "architecture-model.js"]) {
  vm.runInNewContext(readFileSync(path.join(root, file), "utf8"), context, { filename: file });
}
const scopes = context.window.VNG_ARCHITECTURE;
const model = context.window.VNG_MAP_MODEL;
const trees = new Map(Array.from(scopes, scope => [scope.id, model.index(scope.root)]));
const compact = text => text.replace(/\s+/g, "");

// C++ source checks. Comments and string/character literals are blanked (same
// length) so their braces cannot unbalance a body; a quote after a digit is a
// C++14 digit separator (1'000), not a literal.
const texts = new Map();
const read = file => texts.get(file) ?? texts.set(file, readFileSync(path.resolve(repository, file), "utf8")).get(file);
function blank(text) {
  const spaces = (from, to) => text.slice(from, to).replace(/[^\n]/g, " ");
  let out = "";
  for (let i = 0; i < text.length;) {
    const pair = text.slice(i, i + 2);
    let end;
    if (pair === "//" || pair === "/*") {
      end = pair === "//" ? text.indexOf("\n", i) : text.indexOf("*/", i + 2) + 2;
      if (end < 2 || (pair === "//" && end < 0)) end = text.length;
      out += spaces(i, end);
    } else if (text[i] === '"' && /(^|[^\w])(u8|u|U|L)?R$/.test(text.slice(Math.max(0, i - 3), i))) {
      const open = text.indexOf("(", i), delimiter = text.slice(i + 1, open);
      end = text.indexOf(`)${delimiter}"`, open) + delimiter.length + 2;
      out += spaces(i, end);
    } else if (text[i] === '"' || (text[i] === "'" && !/(^|[^\w.])\d[\w.']*$/.test(text.slice(Math.max(0, i - 40), i)))) {
      for (end = i + 1; end < text.length && text[end] !== text[i] && text[end] !== "\n";) end += text[end] === "\\" ? 2 : 1;
      end = Math.min(end + 1, text.length);
      out += spaces(i, end);
    } else {
      out += text[i];
      end = i + 1;
    }
    i = end;
  }
  return out;
}
// The brace-balanced body after a definition head such as "class X final" or
// "int run(...)", skipping forward declarations. Undefined when absent.
function body(file, head) {
  const code = blank(read(file));
  const words = head.trim().split(/\s+/).map(word => word.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"));
  const definition = new RegExp(`${words.join("\\s+")}${/\w$/.test(head) ? "\\b" : ""}[^;{}]*\\{`, "g");
  for (const match of code.matchAll(definition)) {
    const start = match.index + match[0].length;
    for (let i = start, depth = 1; i < code.length; i++) {
      if (code[i] === "{") depth++;
      else if (code[i] === "}" && --depth === 0) return code.slice(start, i);
    }
  }
  return undefined;
}
// A declaration at the top level of a body: a member, not a local of a method
// or of a nested block. Whitespace is ignored.
function declares(code, declaration) {
  const text = compact(code), wanted = compact(declaration);
  for (let at = text.indexOf(wanted); at >= 0; at = text.indexOf(wanted, at + 1)) {
    let depth = 0;
    for (const character of text.slice(0, at)) depth += character === "{" ? 1 : character === "}" ? -1 : 0;
    if (depth === 0) return true;
  }
  return false;
}
const owned = ["editor", "worker"].map(id => [id, trees.get(id)]);

// CMake targets and direct links, read like the guide's content test reads them.
const cmake = readFileSync(path.join(repository, "CMakeLists.txt"), "utf8").replace(/#[^\n]*/g, "");
const cmakeTargets = new Map();
for (const [, kind, name, rest] of cmake.matchAll(/add_(library|executable)\(\s*([\w:]+)\s+([^)]*)\)/g)) {
  if (!name.includes("::") && !/^\s*(ALIAS|IMPORTED)\b/.test(rest)) cmakeTargets.set(name, kind);
}
const cmakeLinks = new Map();
for (const [, call] of cmake.matchAll(/target_link_libraries\s*\(([^)]*)\)/g)) {
  const [target, ...tokens] = call.trim().split(/\s+/);
  let visibility = "PUBLIC";
  for (const token of tokens) {
    if (["PUBLIC", "PRIVATE", "INTERFACE"].includes(token)) visibility = token;
    else (cmakeLinks.get(target) ?? cmakeLinks.set(target, []).get(target)).push({ target: token, visibility });
  }
}
const isTest = target => /_tests$/.test(target);
const moduleScope = scopes.find(scope => scope.id === "modules");
const modules = () => [...trees.get("modules").nodes.values()].filter(node => node.component);

test("architecture covers the editor, worker and module catalog without ambiguous IDs", () => {
  assert.deepEqual(Array.from(trees.keys()), ["editor", "worker", "modules"]);
  assert.ok([...trees.values()].reduce((count, tree) => count + tree.nodes.size, 0) >= 100);
  for (const scope of scopes) {
    const tree = trees.get(scope.id);
    assert.equal(tree.parents.size, tree.nodes.size - 1);
    for (const id of scope.expanded) assert.ok(tree.nodes.get(id)?.children.length, id);
    assert.ok(existsSync(path.resolve(root, scope.guide)));
    for (const node of tree.nodes.values()) {
      assert.match(node.id, /^[a-z][a-z0-9-]*$/);
      assert.ok(node.title && node.symbol && node.role && node.summary && node.detail, node.id);
      assert.ok(node.sources.length, node.id);
      for (const source of node.sources) {
        const file = path.resolve(repository, source);
        assert.ok(file.startsWith(repository + path.sep) && existsSync(file), `${node.id}: ${source}`);
      }
      for (const ref of node.references || []) assert.ok(trees.get(ref.scope)?.nodes.has(ref.id), `${node.id}: broken reference ${ref.id}`);
      if (node.guide) {
        assert.ok(existsSync(path.resolve(root, node.guide.split("#")[0])));
      }
    }
  }
});

test("every editor and worker class is witnessed by its own definition", () => {
  for (const [, tree] of owned) {
    for (const node of tree.nodes.values()) {
      if (node.edge === "group") {
        assert.ok(!node.witness, `${node.id}: a named group has no single definition`);
        continue;
      }
      assert.ok(node.witness, `${node.id}: add a witness to declarations`);
      assert.ok(body(node.sources[0], node.witness) !== undefined, `${node.id}: "${node.witness}" defines nothing in ${node.sources[0]}`);
      const named = node.witness.match(/^(?:class|struct)\s+([\w:]+)/)?.[1];
      if (node.role === "Host") {
        assert.ok(!named, `${node.id}: a host is a function`);
        continue;
      }
      assert.ok(named, `${node.id}: witness its class or struct`);
      assert.ok(node.symbol.includes(named), `${node.id}: ${node.symbol} does not name ${named}`);
      if (/^[A-Za-z_]\w*(::[A-Za-z_]\w*)*$/.test(node.symbol)) {
        assert.equal(named.split("::").at(-1), node.symbol.split("::").at(-1), `${node.id}: symbol and class disagree`);
      }
    }
  }
});

test("every editor and worker parent link is a member declared in its owner", () => {
  let links = 0;
  for (const [, tree] of owned) {
    for (const node of tree.nodes.values()) {
      const parent = tree.nodes.get(tree.parents.get(node.id));
      if (!parent) continue;
      const head = node.memberOf || parent.witness, file = parent.sources[0];
      const owner = body(file, head);
      assert.ok(owner !== undefined, `${node.id}: its owner ${parent.id} has no body "${head}" in ${file}`);
      const declarations = node.edge === "group" ? node.members : [node.member];
      assert.ok(declarations?.length && declarations.every(Boolean),
        `${node.id}: declarations must name the ${node.edge === "group" ? "members it groups" : "member"} in ${parent.id}`);
      for (const declaration of declarations) {
        assert.ok(declares(owner, declaration), `${node.id}: "${declaration}" is not a member of ${head} (${file})`);
        links++;
      }
    }
  }
  assert.ok(links >= [...owned].reduce((count, [, tree]) => count + tree.parents.size, 0));
  assert.ok(!trees.get("editor").nodes.has("worker"), "A worker is another process, not an editor-owned C++ child");
  assert.equal(trees.get("worker").parents.get("gl-session"), "worker-host", "Worker borrows, not owns, the GL session");
  assert.match(trees.get("worker").nodes.get("worker").borrows.join(" "), /Device& and Window&/);
});

test("declarations only describe nodes that are on the map", () => {
  const declarations = context.window.VNG_ARCHITECTURE_DECLARATIONS;
  assert.deepEqual(Object.keys(declarations), owned.map(([id]) => id));
  for (const [id, tree] of owned) {
    for (const key of Object.keys(declarations[id])) assert.ok(tree.nodes.has(key), `${id}: stale declaration ${key}`);
  }
});

test("the source scanner finds members, not comments, literals or method locals", () => {
  const source = `class A final : public B { // class A {
    void f() { Local unused; auto text = "}}{"; char brace = '}'; auto raw = R"x(})x"; }
    std::optional<Owned> owned_{1'000, 200'000}; /* } */
  };`;
  const code = blank(source);
  assert.equal(code.length, source.length);
  assert.equal(code.match(/[{}]/g).join(""), "{{}{}}");
  const inside = code.slice(code.indexOf("{") + 1, code.lastIndexOf("}"));
  assert.ok(declares(inside, "std::optional<Owned> owned_{1'000, 200'000};"));
  assert.ok(!declares(inside, "Local unused;"));
  assert.ok(!declares(inside, "class A {"));
});

test("application widgets and drawing sit directly under the editor host", () => {
  const tree = trees.get("editor");
  assert.ok(!tree.nodes.has("shell"), "Do not invent an ApplicationShell ownership layer");
  for (const id of ["screen", "ui-draw", "shell-source"]) {
    assert.equal(tree.parents.get(id), "editor-host", id);
  }
  for (const id of ["screen", "ui-draw"]) assert.notEqual(tree.nodes.get(id).edge, "group");
  assert.equal(tree.nodes.get("shell-source").edge, "group", "The toolbar/dialog aggregate remains an explicit source grouping");
});

test("module map reuses the verified CMake catalog and marks grouping honestly", () => {
  const flatten = nodes => nodes.flatMap(node => [node, ...flatten(node.children || [])]);
  const catalog = flatten(context.window.VNG_GUIDE).filter(node => node.component);
  const modules = [...trees.get("modules").nodes.values()].filter(node => node.component);
  assert.equal(modules.length, catalog.length);
  for (const entry of catalog) {
    const mapped = modules.find(node => node.id === entry.id);
    assert.equal(mapped.component, entry.component, "Use one dependency data source");
    assert.equal(mapped.references.length, entry.component.dependencies.length);
  }
  for (const node of trees.get("modules").nodes.values()) {
    if (node.id !== "modules-root") assert.equal(node.edge, "group", node.id);
  }
});

test("every CMake library is on the module map; other executables are listed programs", () => {
  const mapped = new Map(modules().map(node => [node.component.target, node]));
  for (const [target, kind] of cmakeTargets) {
    if (kind === "library") assert.ok(mapped.has(target), `${target}: add this CMake library to the catalog in codebase.js`);
    else if (!isTest(target)) {
      assert.notEqual(mapped.has(target), moduleScope.programs.some(program => program.target === target),
        `${target}: give this executable a card or list it in programs, exactly once`);
    }
  }
  for (const target of mapped.keys()) assert.ok(cmakeTargets.has(target), `${target}: not a CMake target`);
  for (const program of moduleScope.programs) {
    assert.equal(cmakeTargets.get(program.target), "executable", `${program.target}: not a CMake executable`);
    const links = cmakeLinks.get(program.target) || [];
    assert.deepEqual([...program.links].sort(), links.map(link => link.target).sort(), `${program.target}: links differ from CMake`);
    assert.ok(links.every(link => link.visibility === "PRIVATE"), `${program.target}: programs link PRIVATE`);
  }
});

test("Used by shows exactly the non-test CMake targets that link each module", () => {
  for (const node of modules()) {
    const target = node.component.target, shown = new Map(), actual = new Map();
    for (const user of modules()) {
      for (const dependency of user.component.dependencies) if (dependency.target === target) shown.set(user.component.target, dependency.visibility);
    }
    for (const program of moduleScope.programs) if (program.links.includes(target)) shown.set(program.target, "PRIVATE");
    for (const [user, links] of cmakeLinks) {
      if (isTest(user)) continue;
      for (const link of links) if (link.target === target) (actual.get(user) ?? actual.set(user, new Set()).get(user)).add(link.visibility);
    }
    assert.deepEqual([...shown.keys()].sort(), [...actual.keys()].sort(), `${target}: Used by differs from CMake`);
    // A target defined in alternative branches (vng_glsl) may declare both scopes.
    for (const [user, visibility] of shown) assert.ok(actual.get(user).has(visibility), `${target}: ${user} does not link it ${visibility}`);
  }
});

test("module source links are files of that module's own target", () => {
  const layering = readFileSync(path.join(repository, "tests/layering/layering.test.cjs"), "utf8");
  const table = name => {
    const literal = layering.match(new RegExp(`const ${name} = (\\{[\\s\\S]*?\\});`));
    assert.ok(literal, `layering.test.cjs no longer defines ${name}`);
    return vm.runInNewContext(`(${literal[1]})`);
  };
  const moduleTargets = table("moduleTargets"), fileTargets = table("fileTargets");
  let checked = 0;
  for (const node of modules()) {
    for (const source of node.sources) {
      const directory = source.match(/^(?:include\/vng|src)\/([a-z_]+)(?:\/|$)/)?.[1];
      if (!directory) continue;
      const owner = fileTargets[source] ?? moduleTargets[directory];
      assert.equal(owner, node.component.target, `${node.component.target} links ${source}, which belongs to ${owner}`);
      checked++;
    }
  }
  assert.ok(checked >= 50);
});

test("duplicate ownership or cyclic input is rejected rather than silently traversed", () => {
  const child = { id: "same", children: [] };
  assert.throws(() => model.index({ id: "root", children: [child, child] }), /Duplicate/);
  child.children.push(child);
  assert.throws(() => model.index(child), /Duplicate/);
});

test("deep search finds collapsed classes, files and multiple-word descriptions", () => {
  const tree = trees.get("editor");
  assert.ok(model.search(tree, "CameraWalkLogic").some(node => node.id === "camera-walk"));
  assert.ok(model.search(tree, "camera_pointer_logic.hpp").some(node => node.id === "camera-pointer"));
  assert.ok(model.search(tree, "camera navigation").some(node => node.id === "navigation"));
  assert.equal(model.search(tree, "<script>not-a-component</script>").length, 0);
  assert.equal(model.search(tree, "   ").length, 0);
});

test("revealing a bookmark opens only its ancestors; collapse retains valid selection", () => {
  const tree = trees.get("editor"), expanded = new Set(), leaf = "camera-walk";
  const route = Array.from(model.path(tree, leaf));
  assert.equal(route[0], tree.root.id);
  assert.equal(route.at(-1), leaf);
  for (let i = 1; i < route.length; i++) assert.equal(tree.parents.get(route[i]), route[i - 1]);
  model.reveal(tree, expanded, leaf);
  assert.deepEqual([...expanded], route.slice(0, -1));
  assert.ok(model.layout(tree, expanded).nodes.some(p => p.node.id === leaf));
  const ancestor = route.at(-3), unrelated = [...tree.nodes.values()].find(node => node.children.length && !route.includes(node.id)).id;
  assert.equal(model.collapse(tree, expanded, ancestor, leaf), ancestor);
  assert.equal(model.collapse(tree, expanded, unrelated, leaf), leaf);
  assert.equal(model.path(tree, "missing").length, 0);
  model.reveal(tree, expanded, "missing"); // Invalid bookmarks do not poison expansion.
  assert.ok(!expanded.has("missing"));
});

function checkLayout(tree, expanded) {
  const first = model.layout(tree, expanded), second = model.layout(tree, expanded);
  assert.deepEqual(first, second, "Layout must be stable, with no force-simulation jitter");
  assert.equal(first.edges.length, first.nodes.length - 1);
  for (const p of first.nodes) {
    assert.ok(Number.isFinite(p.x) && Number.isFinite(p.y));
    assert.ok(p.x >= 0 && p.y >= 0);
    assert.ok(p.x + first.width <= first.bounds.width);
    assert.ok(p.y + first.height <= first.bounds.height);
    for (const q of first.nodes) if (p !== q) {
      assert.ok(p.x + first.width <= q.x || q.x + first.width <= p.x ||
        p.y + first.height <= q.y || q.y + first.height <= p.y, `${p.node.id} overlaps ${q.node.id}`);
    }
  }
  for (const edge of first.edges) {
    assert.ok(edge.from.x < edge.to.x);
    assert.equal(tree.parents.get(edge.to.node.id), edge.from.node.id);
  }
}
test("overview, fully expanded and focused trees have deterministic nonoverlapping layouts", () => {
  for (const scope of scopes) {
    const tree = trees.get(scope.id);
    checkLayout(tree, new Set(scope.expanded));
    checkLayout(tree, new Set(tree.nodes.keys()));
    checkLayout(tree, new Set());
    for (const node of tree.nodes.values()) if (node.children.length) {
      checkLayout(model.index(node), new Set([node.id]));
    }
  }
});

test("fit stays within the canvas and anchored zoom preserves the cursor's world point", () => {
  for (const scope of scopes) {
    for (const expanded of [new Set(scope.expanded), new Set(trees.get(scope.id).nodes.keys())]) {
      const bounds = model.layout(trees.get(scope.id), expanded).bounds;
      for (const viewport of [{ width: 1080, height: 740 }, { width: 360, height: 310 }, { width: 320, height: 160 }]) {
        const fit = model.fit(bounds, viewport);
        assert.ok(fit.scale > 0 && fit.scale <= 1);
        assert.ok(fit.x >= 0 && fit.y >= 0);
        assert.ok(fit.x + bounds.width * fit.scale <= viewport.width);
        assert.ok(fit.y + bounds.height * fit.scale <= viewport.height);
      }
    }
  }
  const view = { x: -54, y: 129, scale: .7 }, cursor = { x: 153, y: 240 };
  for (const factor of [.0001, .8, 1.2, 9000]) {
    const next = model.zoom(view, factor, cursor);
    assert.ok(next.scale >= .01 && next.scale <= 1.8);
    for (const axis of ["x", "y"]) assert.ok(Math.abs((cursor[axis] - next[axis]) / next.scale -
      (cursor[axis] - view[axis]) / view.scale) < 1e-9);
  }
});

test("viewer keeps offline assets, accessible controls and no raw HTML insertion", () => {
  const html = readFileSync(path.join(root, "architecture.html"), "utf8");
  for (const [, file] of html.matchAll(/(?:src|href)="([^"]+)"/g)) {
    if (file.startsWith("#")) continue;
    assert.ok(!/^[a-z]+:/i.test(file) && existsSync(path.resolve(root, file)), file);
  }
  assert.match(html, /role="tree"/);
  assert.match(html, /aria-live="polite"/);
  assert.match(html, /<noscript>/);
  for (const file of ["architecture.js", "architecture-model.js", "architecture-data.js"]) {
    const js = readFileSync(path.join(root, file), "utf8");
    assert.doesNotMatch(js, /\b(?:fetch|XMLHttpRequest)\s*\(|\.innerHTML\s*=|\beval\s*\(/);
    new vm.Script(js, { filename: file });
  }
});
