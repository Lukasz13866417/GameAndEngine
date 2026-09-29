// Optional real-browser coverage, called by browser.cjs. No viewer test hooks.
const assert = require("node:assert/strict");
const path = require("node:path");

module.exports = async function architectureBrowser(page, url, output) {
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto(url("architecture.html"));
  const selected = () => page.locator(".node[aria-selected='true']");
  assert.equal(await selected().getAttribute("data-id"), "editor-host");
  assert.match(await page.locator("#scope-subtitle").innerText(), /UI-process ownership/);
  await page.screenshot({ path: path.join(output, "architecture-editor.png") });

  // Search reaches hidden descendants, establishes a bookmark and focuses the
  // actual tree item. Back to the initial empty hash must restore the root.
  await page.keyboard.press("/");
  await page.locator("#map-search").fill("CameraWalkLogic");
  await page.keyboard.press("Enter");
  await page.waitForURL(/#editor\/camera-walk$/);
  assert.equal(await selected().getAttribute("data-id"), "camera-walk");
  assert.equal(await selected().evaluate(node => node === document.activeElement), true);
  await page.goBack();
  await page.waitForFunction(() => document.querySelector(".node[aria-selected='true']")?.dataset.id === "editor-host");
  await page.goForward();
  await page.waitForFunction(() => document.querySelector(".node[aria-selected='true']")?.dataset.id === "camera-walk");
  await selected().focus();
  await page.keyboard.press("ArrowLeft");
  assert.equal(await selected().getAttribute("data-id"), "navigation");
  await page.keyboard.press("ArrowLeft");
  assert.equal(await selected().getAttribute("aria-expanded"), "false");
  assert.equal(await page.locator("#map-node-camera-walk").count(), 0);
  await page.keyboard.press("ArrowRight");
  assert.equal(await selected().getAttribute("aria-expanded"), "true");
  await page.locator("#details").getByRole("button", { name: "Focus branch", exact: true }).click();
  assert.equal(await page.locator(".node").count(), 3);
  await page.screenshot({ path: path.join(output, "architecture-navigation-branch.png") });
  await page.locator("#leave-branch").click();
  assert.ok(await page.locator(".node").count() > 3);

  // Search across scopes, independent lifetimes, and direct build links.
  await page.locator("#map-search").fill("Rgba8ReadbackQueue");
  await page.locator("#map-search").press("Enter");
  await page.waitForURL(/#worker\/readback$/);
  await page.reload();
  assert.equal(await selected().getAttribute("data-id"), "readback");
  assert.match(await page.locator("#details").innerText(), /latest completed image/);
  await page.locator("#scopes").getByRole("button", { name: "Engine modules", exact: true }).click();
  await page.locator("#map-search").fill("vng_opengl");
  await page.locator("#search-results").getByRole("button", { name: /OpenGL device & resources/ }).click();
  assert.match(await page.locator("#details").innerText(), /Direct dependencies/);
  await page.locator("#details").getByRole("button", { name: /PUBLIC · vng_glsl/ }).click();
  await page.waitForURL(/#modules\/code-glsl$/);
  assert.match(await page.locator("#details").innerText(), /Used by/);
  assert.equal(await page.locator("#details .source-link").first().getAttribute("target"), "_blank");

  await page.locator("#map-search").fill("<img src=x>");
  assert.match(await page.locator("#search-results").innerText(), /No matches/);
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
  assert.equal(await page.locator(".node").count(), 48);
  await page.locator("#overview").click();
  assert.equal(await page.locator(".node").count(), 11);
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
  assert.equal(await selected().getAttribute("data-id"), "editor-host");
};
