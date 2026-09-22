"use strict";
// Source-level layering checks for the engine libraries.
//
// Every directory under include/vng/ and src/ belongs to exactly one CMake
// target (the table below). A file may include <vng/x/...> only when its own
// target links x, directly or through PUBLIC/INTERFACE links, and the module
// graph formed by those includes must be a tree with no cycles. The check reads
// the sources and CMakeLists.txt, so it needs no build.
const test = require("node:test");
const assert = require("node:assert/strict");
const { readFileSync, readdirSync, statSync } = require("node:fs");
const path = require("node:path");

const repository = path.resolve(__dirname, "..", "..");

// Directory -> target. Directories without a compiled target of their own
// (core, input) are header-only parts of vng_core.
const moduleTargets = {
  analysis: "vng_analysis", bloom_opengl: "vng_bloom_opengl", content: "vng_content", core: "vng_core",
  editor: "vng_editor", gfx: "vng_gfx", glfw_opengl: "vng_glfw_opengl", glsl: "vng_glsl", input: "vng_core",
  opengl: "vng_opengl", providers: "vng_providers", render: "vng_render", render_opengl: "vng_render_opengl",
  resources: "vng_resources", resources_opengl: "vng_resources_opengl", rig: "vng_rig", rig_opengl: "vng_rig_opengl",
  shader: "vng_shader", spatial: "vng_spatial", text: "vng_text", text_opengl: "vng_text_opengl",
  timeline: "vng_timeline", ui: "vng_ui", ui_opengl: "vng_ui_opengl", window: "vng_window_glfw",
};
// Files that belong to a different target than their directory suggests.
const fileTargets = {
  "include/vng/editor/preview.hpp": "vng_editor_preview",
  "src/editor/preview.cpp": "vng_editor_preview",
};
// Private headers reached across target directories with a relative include.
// Each entry is a dependency that CMake cannot see; shrink this list, never grow it.
const privateReachIns = [
  ["src/bloom_opengl/bloom.cpp", "src/opengl/gl_error.hpp"],
  ["src/glfw_opengl/glfw_opengl.cpp", "src/window/glfw_access.hpp"],
  ["src/resources_opengl/mesh_renderer.cpp", "src/opengl/gl_error.hpp"],
  ["src/rig_opengl/skinned_mesh_renderer.cpp", "src/opengl/gl_error.hpp"],
  ["src/text_opengl/text_renderer.cpp", "src/opengl/gl_error.hpp"],
  ["src/ui_opengl/ui_renderer.cpp", "src/opengl/gl_error.hpp"],
];

const walk = (directory, out = []) => {
  for (const name of readdirSync(directory)) {
    const file = path.join(directory, name);
    if (statSync(file).isDirectory()) walk(file, out);
    else if (/\.(hpp|cpp|h)$/.test(name)) out.push(file);
  }
  return out;
};
const relative = file => path.relative(repository, file).split(path.sep).join("/");
const files = [...walk(path.join(repository, "include", "vng")), ...walk(path.join(repository, "src"))].map(relative);

const moduleOf = file => file.split("/")[file.startsWith("include/") ? 2 : 1];
const targetOf = file => {
  if (fileTargets[file]) return fileTargets[file];
  const target = moduleTargets[moduleOf(file)];
  assert.ok(target, `${file}: add its directory to moduleTargets`);
  return target;
};
const includesOf = file => {
  const text = readFileSync(path.join(repository, file), "utf8");
  const engine = [...text.matchAll(/^\s*#\s*include\s*<vng\/([a-z_]+)\//gm)].map(match => match[1]);
  const relativeIncludes = [...text.matchAll(/^\s*#\s*include\s*"(\.\.\/[^"]+)"/gm)]
    .map(match => path.posix.normalize(path.posix.join(path.posix.dirname(file), match[1])));
  return { engine, relativeIncludes };
};

// CMake links. PUBLIC and INTERFACE links propagate to consumers of a target's
// headers; PRIVATE links are available only to the target's own sources.
const cmake = readFileSync(path.join(repository, "CMakeLists.txt"), "utf8").replace(/#[^\n]*/g, "");
const links = new Map();
for (const [, body] of cmake.matchAll(/target_link_libraries\s*\(([^)]*)\)/g)) {
  const [target, ...tokens] = body.trim().split(/\s+/);
  if (!links.has(target)) links.set(target, []);
  let visibility = "PUBLIC";
  for (const token of tokens) {
    if (["PUBLIC", "PRIVATE", "INTERFACE"].includes(token)) visibility = token;
    else links.get(target).push({ target: token, visibility });
  }
}
const reachable = (target, includePrivate) => {
  const seen = new Set([target]);
  const queue = [{ target, first: true }];
  while (queue.length) {
    const { target: current, first } = queue.shift();
    for (const link of links.get(current) || []) {
      if (link.visibility === "PRIVATE" && !(first && includePrivate)) continue;
      if (!seen.has(link.target)) { seen.add(link.target); queue.push({ target: link.target, first: false }); }
    }
  }
  return seen;
};

test("every module directory maps to a target that CMake declares", () => {
  for (const target of new Set(Object.values(moduleTargets))) {
    assert.match(cmake, new RegExp(`add_library\\(${target}\\s`), `${target} is not a CMake target`);
  }
  const directories = new Set(files.map(moduleOf));
  for (const directory of directories) assert.ok(moduleTargets[directory], `include/src directory ${directory} has no target`);
});

test("a file includes only modules its target links", () => {
  const problems = [];
  for (const file of files) {
    const target = targetOf(file);
    const allowed = reachable(target, file.startsWith("src/"));
    for (const module of new Set(includesOf(file).engine)) {
      const needed = moduleTargets[module];
      assert.ok(needed, `${file}: unknown module vng/${module}`);
      if (!allowed.has(needed)) problems.push(`${file} (${target}) includes <vng/${module}/...> but ${target} does not link ${needed}`);
    }
  }
  assert.deepEqual(problems, []);
});

test("the module graph is a tree, not a cycle", () => {
  const edges = new Map();
  for (const file of files) {
    const from = moduleOf(file);
    for (const to of includesOf(file).engine) if (to !== from) (edges.get(from) || edges.set(from, new Set()).get(from)).add(to);
  }
  const state = new Map();
  const stack = [];
  const cycles = [];
  const visit = node => {
    state.set(node, "active"); stack.push(node);
    for (const next of edges.get(node) || []) {
      if (state.get(next) === "active") cycles.push([...stack.slice(stack.indexOf(next)), next].join(" -> "));
      else if (!state.has(next)) visit(next);
    }
    stack.pop(); state.set(node, "done");
  };
  for (const node of edges.keys()) if (!state.has(node)) visit(node);
  assert.deepEqual(cycles, []);
});

test("private headers are not reached across target directories, beyond the listed exceptions", () => {
  const found = [];
  for (const file of files) {
    for (const included of includesOf(file).relativeIncludes) {
      if (moduleOf(included) !== moduleOf(file)) found.push([file, included]);
    }
  }
  assert.deepEqual(found.sort(), [...privateReachIns].sort());
});
