// Optional real-browser coverage, called by browser.cjs. No viewer test hooks:
// expected paths and node counts come from the same data files, loaded here.
const assert = require("node:assert/strict");
const path = require("node:path");
const { readFileSync } = require("node:fs");
const vm = require("node:vm");

const root = path.resolve(__dirname, "..");
const data = { window: {} };
for (const file of ["data.js", "codebase.js", "architecture-data.js", "architecture-model.js"]) {
  vm.runInNewContext(readFileSync(path.join(root, file), "utf8"), data, { filename: file });
}
const model = data.window.VNG_MAP_MODEL;
const maps = new Map(Array.from(data.window.VNG_ARCHITECTURE, scope => [scope.id, { scope, tree: model.index(scope.root) }]));
const editor = maps.get("editor");
// Nodes the page should draw for a scope, given its expanded branches.
const visible = (tree, expanded) => model.layout(tree, new Set(expanded)).nodes.length;
// Text as authored, independent of CSS text-transform (innerText applies it).
const text = locator => locator.textContent();
const headings = page => page.locator("#details h3").allTextContents();

module.exports = async function architectureBrowser(page, url, output) {
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto(url("architecture.html"));
  const selected = () => page.locator(".node[aria-selected='true']");
  assert.equal(await selected().getAttribute("data-id"), editor.scope.root.id);
  assert.equal(await text(page.locator("#scope-subtitle")), editor.scope.subtitle);
  assert.equal(await page.locator(".node").count(), visible(editor.tree, editor.scope.expanded));
  await page.screenshot({ path: path.join(output, "architecture-editor.png") });

  // Search reaches hidden descendants, establishes a bookmark and focuses the
  // actual tree item. Back to the initial empty hash must restore the root.
  const walk = "camera-walk", owner = editor.tree.parents.get(walk);
  await page.keyboard.press("/");
  await page.locator("#map-search").fill("CameraWalkLogic");
  await page.keyboard.press("Enter");
  await page.waitForURL(new RegExp(`#editor/${walk}$`));
  assert.equal(await selected().getAttribute("data-id"), walk);
  assert.equal(await selected().evaluate(node => node === document.activeElement), true);
  await page.goBack();
  await page.waitForFunction(id => document.querySelector(".node[aria-selected='true']")?.dataset.id === id, editor.scope.root.id);
  await page.goForward();
  await page.waitForFunction(id => document.querySelector(".node[aria-selected='true']")?.dataset.id === id, walk);
  await selected().focus();
  await page.keyboard.press("ArrowLeft");
  assert.equal(await selected().getAttribute("data-id"), owner);
  await page.keyboard.press("ArrowLeft");
  assert.equal(await selected().getAttribute("aria-expanded"), "false");
  assert.equal(await page.locator(`#map-node-${walk}`).count(), 0);
  await page.keyboard.press("ArrowRight");
  assert.equal(await selected().getAttribute("aria-expanded"), "true");
  await page.locator("#details").getByRole("button", { name: "Focus branch", exact: true }).click();
  const revealed = [...editor.scope.expanded, ...model.path(editor.tree, walk).slice(0, -1)];
  assert.equal(await page.locator(".node").count(), visible(model.index(editor.tree.nodes.get(owner)), revealed));
  await page.screenshot({ path: path.join(output, "architecture-navigation-branch.png") });
  await page.locator("#leave-branch").click();
  assert.equal(await page.locator(".node").count(), visible(editor.tree, revealed));

  // Search across scopes, independent lifetimes, and direct build links.
  const worker = maps.get("worker");
  await page.locator("#map-search").fill("Rgba8ReadbackQueue");
  await page.locator("#map-search").press("Enter");
  await page.waitForURL(/#worker\/readback$/);
  await page.reload();
  assert.equal(await selected().getAttribute("data-id"), "readback");
  assert.ok((await text(page.locator("#details"))).includes(worker.tree.nodes.get("readback").detail));
  await page.locator("#scopes").getByRole("button", { name: "Engine modules", exact: true }).click();
  await page.locator("#map-search").fill("vng_opengl");
  await page.locator("#search-results").getByRole("button", { name: /OpenGL device & resources/ }).click();
  assert.ok((await headings(page)).includes("Direct dependencies"));
  await page.locator("#details").getByRole("button", { name: /PUBLIC · vng_glsl/ }).click();
  await page.waitForURL(/#modules\/code-glsl$/);
  assert.ok((await headings(page)).includes("Used by"));
  assert.equal(await page.locator("#details .source-link").first().getAttribute("target"), "_blank");

  await page.locator("#map-search").fill("<img src=x>");
  assert.match(await text(page.locator("#search-results")), /No matches/);
  assert.equal(await page.locator("#search-results img").count(), 0);
  await page.locator("#map-search").press("Escape");
  assert.equal(await page.locator("#search-results").isVisible(), false);

  await page.locator("#scopes").getByRole("button", { name: "Editor", exact: true }).click();
  await page.locator("#overview").click();
  const beforeZoom = await page.locator("#zoom-label").innerText();
  await page.locator("#zoom-in").click();
  await page.waitForFunction(before => document.querySelector("#zoom-label").textContent !== before, beforeZoom);
  await page.locator("#fit").click();
  const bounds = await page.locator("#map").boundingBox();
  const point = { x: bounds.x + bounds.width - 8, y: bounds.y + 8 }; // Empty edge, outside fitted cards.
  const beforePan = await page.locator("#plane").getAttribute("style");
  await page.mouse.move(point.x, point.y);
  await page.mouse.down();
  await page.mouse.move(point.x - 85, point.y + 55, { steps: 8 });
  await page.mouse.up();
  await page.waitForFunction(before => document.querySelector("#plane").getAttribute("style") !== before, beforePan);
  const beforeWheel = await page.locator("#zoom-label").innerText();
  await page.mouse.wheel(0, -150);
  await page.waitForFunction(before => document.querySelector("#zoom-label").textContent !== before, beforeWheel);
  await page.locator("#expand-all").click();
  assert.equal(await page.locator(".node").count(), editor.tree.nodes.size);
  await page.locator("#overview").click();
  assert.equal(await page.locator(".node").count(), visible(editor.tree, editor.scope.expanded));
  await page.locator("#details-toggle").click();
  assert.equal(await page.locator("#inspector").isVisible(), false);
  assert.equal(await page.locator("#details-toggle").getAttribute("aria-expanded"), "false");
  await page.locator("#details-toggle").click();

  for (const width of [375, 768, 1440]) {
    await page.setViewportSize({ width, height: 950 });
    await page.locator("#fit").click();
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
    const canvas = await page.locator("#map").boundingBox();
    const inspector = await page.locator("#inspector").boundingBox();
    if (width === 375) assert.ok(canvas.y + canvas.height <= inspector.y);
    else assert.ok(canvas.x + canvas.width <= inspector.x);
    await page.screenshot({ path: path.join(output, `architecture-${width}.png`) });
  }
  await page.goto(url("architecture.html") + "#editor/not-a-component");
  assert.equal(await selected().getAttribute("data-id"), editor.scope.root.id);
};
