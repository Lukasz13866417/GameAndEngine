// Optional: node browser.cjs /path/to/playwright [/path/to/chromium]
// Isolated browser, file:// only, no personal profile or local server.
const assert = require("node:assert/strict");
const path = require("node:path");
const { mkdtempSync, readFileSync } = require("node:fs");
const { tmpdir } = require("node:os");
const { pathToFileURL } = require("node:url");
const { chromium } = require(process.argv[2] || "playwright");
const vm = require("node:vm");
const root = path.resolve(__dirname, "..");
const data = { window: {} };
for (const file of ["data.js", "codebase.js", "codebase-details.js", "walkthrough.js", "pages.js"]) {
  vm.runInNewContext(readFileSync(path.join(root, file), "utf8"), data);
}
const flatten = nodes => nodes.flatMap(node => [node, ...flatten(node.children || [])]);
const nodes = flatten(data.window.VNG_GUIDE);
const pages = data.window.VNG_GUIDE_PAGES;
const routes = data.window.VNG_TOPIC_ROUTES;

(async () => {
  const output = mkdtempSync(path.join(tmpdir(), "vng-guide-browser-"));
  const browser = await chromium.launch({ headless: true, executablePath: process.argv[3] });
  try {
    const page = await browser.newPage({ viewport: { width: 1440, height: 1100 } });
    const errors = [];
    const external = [];
    page.on("pageerror", error => errors.push(error.message));
    page.on("request", request => { if (!request.url().startsWith("file:")) external.push(request.url()); });
    const url = file => pathToFileURL(path.join(root, file)).href;
    const visit = async (file, id) => {
      await page.goto(url(file) + (id ? "#" + id : ""));
      await page.reload();
    };

    // Start here is content, not a tree (with or without JavaScript).
    await visit("index.html");
    assert.equal(await page.locator("details, #tree, .node").count(), 0);
    assert.equal(await page.locator(".overview-grid article").count(), 12);
    assert.equal(await page.locator("#navigation a").count(), 5);
    assert.match(await page.locator("#navigation a[aria-current='page']").innerText(), /^01\s*Start here$/);
    await page.screenshot({ path: path.join(output, "start-desktop.png"), fullPage: true });
    await page.locator("#navigation a[href='editor.html']").click();
    await page.waitForURL(/editor\.html$/);
    assert.equal(await page.locator("#editor").evaluate(e => e.open), true);
    assert.equal(await page.locator("#codebase").count(), 0);
    await page.locator("#navigation a[href='codebase.html']").click();
    await page.waitForURL(/codebase\.html$/);
    assert.equal(await page.locator("#codebase").evaluate(e => e.open), true);
    assert.equal(await page.locator("#editor").count(), 0);

    // Class walkthrough is separate from the CMake/ownership catalog.
    await page.locator("#navigation a[href='walkthrough.html']").click();
    await page.waitForURL(/walkthrough\.html$/);
    assert.equal(await page.locator("#code-components").count(), 0);
    assert.equal(await page.locator("#code-walkthrough").evaluate(e => e.open), true);
    await page.locator("#walk-shader > summary").click();
    await page.locator("#walk-expr > summary").focus();
    await page.keyboard.press("Enter");
    assert.equal(await page.locator("#walk-expr .component-section pre").isVisible(), true);
    assert.match(await page.locator("#walk-expr .ownership").innerText(), /FunctionBuilder/);
    await page.locator("#search").fill("commands.run");
    assert.equal(await page.locator("#walk-commands").isVisible(), true);
    await page.locator("#clear-search").click();
    await page.locator("#navigation a[href='codebase.html']").click();
    await page.waitForURL(/codebase\.html$/);

    // Native keyboard expansion preserves other work on this page.
    await page.locator("#engine > summary").focus();
    await page.keyboard.press("Enter");
    await page.locator("#drawing > summary").click();
    await page.locator("#shader-dsl > summary").focus();
    await page.keyboard.press("Space");
    assert.equal(await page.locator("#shader-dsl").evaluate(e => e.open), true);
    assert.equal(await page.locator("#codebase").evaluate(e => e.open), true);
    await page.locator("#search").fill("intermediate representation");
    assert.match(await page.locator("#search-status").innerText(), /^1 matching topic/);
    assert.equal(await page.locator("#shader-dsl").isVisible(), true);
    assert.equal(await page.locator("#code-components").isVisible(), false);
    assert.equal(await page.locator("#collapse").isDisabled(), true);
    await page.locator("#clear-search").click();
    assert.equal(await page.locator("#shader-dsl").evaluate(e => e.open), true);
    await page.locator("#search").fill("no-such-topic-123");
    assert.equal(await page.locator("#empty").isVisible(), true);
    await page.locator("#search").fill("<img src=x>");
    assert.equal(await page.locator("img").count(), 0);
    await page.keyboard.press("Escape");
    assert.equal(await page.locator("#search").inputValue(), "");

    // Every old single-page bookmark opens its new canonical page/topic.
    for (const node of nodes) {
      await page.goto("about:blank");
      await page.goto(url("index.html") + "#" + node.id);
      const destination = routes.get(node.id);
      await page.waitForURL(url(destination.file) + "#" + node.id);
      await page.locator("#" + node.id).waitFor({ state: "visible" });
      if (await page.locator("#" + node.id).evaluate(e => e.tagName === "DETAILS")) {
        await page.waitForFunction(id => document.activeElement === document.querySelector("#" + id + " > summary"), node.id);
        assert.equal(await page.locator("#" + node.id).evaluate(e => e.open), true, node.id);
      }
      assert.equal(await page.locator("#navigation a[aria-current='page']").getAttribute("href"), destination.file);
    }

    // Cross-page related links and Back work, not just hash-only navigation.
    await visit("editor.html", "viewport");
    await page.locator("#viewport .related a[href='codebase.html#camera']").click();
    await page.waitForURL(/codebase\.html#camera$/);
    await page.goBack();
    await page.waitForURL(/editor\.html#viewport$/);
    await page.waitForFunction(() => document.querySelector("#viewport").open);
    await page.locator("#collapse").click();
    assert.equal(await page.locator(".node[open]").count(), 0);
    await page.evaluate(() => document.querySelector("#viewport .permalink").click());
    await page.waitForFunction(() => document.querySelector("#viewport").open);
    await page.locator("#reset").click();
    await page.waitForURL(/editor\.html#editor$/);
    await page.waitForFunction(() => document.activeElement === document.querySelector("#editor > summary"));
    assert.equal(await page.locator(".node[open]").count(), 1);
    await page.locator("#editor > summary").focus();
    await page.keyboard.press("/");
    assert.equal(await page.locator("#search").evaluate(e => e === document.activeElement), true);
    await page.locator("#search").fill("keyframe");
    assert.equal(await page.locator("#timeline").isVisible(), true);
    await page.keyboard.press("Escape");

    // Direct and derived reverse dependency links still work on Codebase.
    await visit("codebase.html", "code-rig-opengl");
    await page.locator("#code-rig-opengl .dependencies > summary").focus();
    await page.keyboard.press("Enter");
    await page.locator("#code-rig-opengl .dependencies a[href='#code-rig']").click();
    await page.waitForURL(/#code-rig$/);
    await page.locator("#code-rig .dependencies > summary").click();
    assert.equal(await page.locator("#code-rig .dependencies a[href='#code-rig-opengl']").isVisible(), true);
    await page.locator("#search").fill("include/vng/opengl/commands.hpp");
    assert.equal(await page.locator("#code-opengl").isVisible(), true);
    assert.equal(await page.locator("#code-backend-dispatch").isVisible(), true);
    await page.locator("#clear-search").click();
    await visit("codebase.html", "code-editor-app");
    await page.locator("#code-editor-app .dependencies > summary").click();
    assert.match(await page.locator("#code-editor-app .dependencies").innerText(), /Build after \(ordering only; not linked\)/);
    assert.equal(await page.locator("#code-editor-app .dependency-columns a[href='#code-editor-worker']").count(), 0);

    // Source-backed component explanations are visible in expanded cards and searchable.
    await visit("codebase.html", "code-shader");
    assert.equal(await page.locator("#code-shader .component-section").count(), 4);
    assert.match(await page.locator("#code-shader .component-section").first().innerText(), /Copying it copies a handle/);
    await page.screenshot({ path: path.join(output, "shader-api-desktop.png"), fullPage: true });
    await page.locator("#search").fill("palette construction");
    assert.equal(await page.locator("#code-rig-opengl").isVisible(), true);
    assert.match(await page.locator("#search-status").innerText(), /^1 matching topic/);
    await page.locator("#clear-search").click();
    for (const width of [375, 768, 1440]) {
      await page.setViewportSize({ width, height: 950 });
      await visit("codebase.html", "code-render");
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
      await page.screenshot({ path: path.join(output, `renderer-api-${width}.png`), fullPage: true });
    }
    await page.setViewportSize({ width: 1440, height: 1100 });

    // Plain demo: real program and source excerpts, not another concept tree.
    await visit("demo.html");
    assert.equal(await page.locator("details, #tree").count(), 0);
    assert.ok(await page.locator("pre[data-source]").count() >= 6);
    await page.locator(".reading-paths a[href='#diagnostics']").click();
    await page.waitForURL(/demo\.html#diagnostics$/);
    assert.equal(await page.locator("#diagnostics").isVisible(), true);
    await page.screenshot({ path: path.join(output, "demo-desktop.png"), fullPage: true });

    for (const width of [375, 768, 1440]) {
      await page.setViewportSize({ width, height: 950 });
      for (const document of pages) {
        await visit(document.file);
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), true,
          document.file + " overflows at " + width);
        if (width !== 768) {
          await page.screenshot({ path: path.join(output, document.id + "-" + width + ".png"), fullPage: true });
          await page.screenshot({ path: path.join(output, document.id + "-viewport-" + width + ".png") });
        }
      }
      await visit("codebase.html", "code-editor-runtime");
      await page.locator("#code-editor-runtime .dependencies > summary").click();
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), true, "dependency overflow at " + width);
      await page.locator("#code-editor-runtime").screenshot({ path: path.join(output, "dependency-card-" + width + ".png") });
      await visit("walkthrough.html", "walk-commands");
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), true, "class API overflow at " + width);
      await page.locator("#walk-commands").screenshot({ path: path.join(output, "class-api-" + width + ".png") });
    }
    await page.setViewportSize({ width: 1440, height: 1100 });
    await visit("codebase.html", "code-session-owner");
    await page.locator("#code-session-owner").screenshot({ path: path.join(output, "ownership-card-desktop.png") });

    // Unknown/malformed bookmarks do not make either page type crash.
    await visit("index.html", "%invalid");
    await visit("editor.html", "not-a-topic");
    assert.equal(await page.locator("#editor").evaluate(e => e.open), true);
    assert.deepEqual(errors, []);
    assert.deepEqual(external, []);

    const noJs = await browser.newContext({ javaScriptEnabled: false });
    const plain = await noJs.newPage();
    await plain.goto(url("index.html"));
    assert.equal(await plain.locator(".overview-grid article").count(), 12);
    assert.equal(await plain.locator("details").count(), 0);
    await plain.locator("#navigation a[href='demo.html']").click();
    await plain.waitForURL(/demo\.html$/);
    assert.equal(await plain.locator("#demo-shaders").isVisible(), true);
    await noJs.close();

    console.log("PASS: five pages; plain overview/demo with JS disabled; " + nodes.length +
      " legacy bookmarks; cross-page history; dependencies/users; keyboard/search; 375/768/1440px; offline/no page errors.");
    console.log("Screenshots: " + output);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
