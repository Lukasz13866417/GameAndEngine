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

test("architecture covers the editor, worker and module catalog without ambiguous IDs", () => {
  assert.deepEqual(Array.from(trees.keys()), ["editor", "worker", "modules", "animations"]);
  assert.ok([...trees.values()].reduce((count, tree) => count + tree.nodes.size, 0) >= 100);
  for (const scope of scopes) {
    const tree = trees.get(scope.id);
    assert.equal(tree.parents.size, tree.nodes.size - 1);
    for (const id of scope.expanded) assert.ok(tree.nodes.get(id)?.children.length, id);
    assert.ok(existsSync(path.resolve(root, scope.guide.split("#")[0])));
    for (const node of tree.nodes.values()) {
      assert.match(node.id, /^[a-z][a-z0-9-]*$/);
      assert.ok(node.title && node.symbol && node.role && node.summary && node.detail, node.id);
      assert.ok(node.sources.length, node.id);
      for (const source of node.sources) {
        const file = path.resolve(repository, source);
        assert.ok(file.startsWith(repository + path.sep) && existsSync(file), `${node.id}: ${source}`);
      }
      if (node.witness) assert.ok(readFileSync(path.resolve(repository, node.sources[0]), "utf8").includes(node.witness), `${node.id}: stale source witness`);
      for (const ref of node.references || []) assert.ok(trees.get(ref.scope)?.nodes.has(ref.id), `${node.id}: broken reference ${ref.id}`);
      if (node.guide) {
        assert.ok(existsSync(path.resolve(root, node.guide.split("#")[0])));
      }
    }
  }
});

test("ownership edges agree with concrete parent member declarations", () => {
  // Representative edges across every major boundary, including the renamed
  // parent-coordinated UI. These catch drift back to the old sibling-owner map.
  const owners = [
    ["editor", "session", "workspace", "workspace_ui.hpp", "EditingSession editing_;"],
    ["editor", "selection", "workspace", "workspace_ui.hpp", "WorkspaceSelection selection_;"],
    ["editor", "viewport", "workspace", "workspace_ui.hpp", "std::optional<EditingViewportUI> viewport_;"],
    ["editor", "scene-lists", "workspace", "workspace_ui.hpp", "std::optional<SceneLists> lists_;"],
    ["editor", "timeline", "workspace", "workspace_ui.hpp", "std::optional<TimelineEditingUI> timeline_;"],
    ["editor", "state", "session", "editing_session.hpp", "State state_;"],
    ["editor", "document", "state", "project.hpp", "Document document;"],
    ["editor", "private-view", "state", "project.hpp", "ViewportState viewport{};"],
    ["editor", "mesh-editing", "viewport", "workspace_ui.hpp", "MeshEditingUI mesh_;"],
    ["editor", "animation-gizmo", "viewport", "workspace_ui.hpp", "std::optional<AnimationGizmo> animation_gizmo_;"],
    ["editor", "interaction", "viewport", "workspace_ui.hpp", "std::optional<ViewportToolsUI> interaction_;"],
    ["editor", "mesh-tools", "mesh-editing", "mesh_editing_ui.hpp", "MeshToolsUI components_;"],
    ["editor", "mesh-menu", "mesh-tools", "mesh_tools_ui.hpp", "MeshMenu menu_;"],
    ["editor", "navigation", "interaction", "viewport_tools_ui.hpp", "CameraGizmo navigation_;"],
    ["editor", "camera-pointer", "navigation", "camera_gizmo.hpp", "std::array<CameraPointerGizmo,5> pointer_"],
    ["editor", "camera-walk", "navigation", "camera_gizmo.hpp", "CameraWalkGizmo walk_;"],
    ["editor", "timeline-panel", "timeline", "timeline_editing_ui.hpp", "TimelinePanel panel_;"],
    ["editor", "transport", "preview", "preview_logic.hpp", "vng::editor::preview::PreviewSession transport_;"],
    ["editor", "delivery", "preview", "preview_logic.hpp", "PreviewDeliveryLogic updates_;"],
    ["editor", "mailbox", "preview", "preview_logic.hpp", "PreviewMailbox frames_;"],
    ["worker", "programs", "runtime", "runtime.cpp", "MeshPrograms mesh_programs;"],
    ["worker", "readback", "runtime", "runtime.cpp", "std::optional<opengl::Rgba8ReadbackQueue> readback_queue;"]
  ];
  const compact = text => text.replace(/\s+/g, "");
  for (const [scope, child, parent, source, member] of owners) {
    assert.equal(trees.get(scope).parents.get(child), parent, child);
    assert.ok(compact(readFileSync(path.join(repository, "examples/editor", source), "utf8")).includes(compact(member)), `${parent}: ${member}`);
  }
  assert.ok(!trees.get("editor").nodes.has("worker"), "A worker is another process, not an editor-owned C++ child");
  assert.equal(trees.get("worker").parents.get("gl-session"), "worker-host", "Worker borrows, not owns, the GL session");
  assert.match(trees.get("worker").nodes.get("worker").borrows.join(" "), /Device& and Window&/);
});

test("animation forest shows real owned children and parent-controlled evaluation", () => {
  const tree = trees.get("animations");
  const header = readFileSync(path.join(repository,"examples/editor/scene_animation.hpp"),"utf8");
  const implementation = readFileSync(path.join(repository,"examples/editor/scene_animation.cpp"),"utf8");
  for (const [child,parent,member] of [
    ["route","motion","RouteCurve route;"], ["speed","motion","SpeedProfile speed;"],
    ["turbulence","motion","Turbulence turbulence;"], ["motion","departure","ShipMotion motion;"],
    ["follow","departure","CameraFollow follow;"]
  ]) {
    assert.equal(tree.parents.get(child),parent);
    assert.ok(header.includes(member),member);
  }
  assert.equal(tree.parents.get("departure"),"forest");
  assert.equal(tree.parents.get("spin"),"forest");
  assert.match(implementation,/const auto moving=motion\.evaluate\(seconds,duration\);[\s\S]*return \{moving,follow\.evaluate\(moving\)\};/);
  assert.match(tree.nodes.get("forest").detail,/not a live scene inspector/);
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
  const tree = trees.get("editor"), expanded = new Set();
  const expected = ["editor-host", "workspace", "viewport", "interaction", "navigation", "camera-walk"];
  assert.deepEqual(Array.from(model.path(tree, "camera-walk")), expected);
  model.reveal(tree, expanded, "camera-walk");
  assert.deepEqual([...expanded], expected.slice(0, -1));
  assert.ok(model.layout(tree, expanded).nodes.some(p => p.node.id === "camera-walk"));
  assert.equal(model.collapse(tree, expanded, "interaction", "camera-walk"), "interaction");
  assert.equal(model.collapse(tree, expanded, "preview", "camera-walk"), "camera-walk");
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
