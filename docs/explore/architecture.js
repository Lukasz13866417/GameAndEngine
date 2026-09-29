/* No fetches, framework, graph runtime or build step: works from file://. */
(() => {
  "use strict";
  const model = window.VNG_MAP_MODEL;
  const scopes = window.VNG_ARCHITECTURE;
  const states = new Map(scopes.map(scope => [scope.id, {
    scope, tree: model.index(scope.root), expanded: new Set(scope.expanded),
    selected: scope.root.id, focus: scope.root.id, view: null
  }]));
  const $ = id => document.getElementById(id);
  const map = $("map"), plane = $("plane"), nodes = $("nodes"), edges = $("edges");
  const search = $("map-search"), results = $("search-results");
  const mini = $("minimap");
  let state = states.get("editor"), diagram, miniScale = 1, transformFrame = 0;
  let previousSize = { width: map.clientWidth, height: map.clientHeight };
  let drag = null, suppressClick = false;
  const pointers = new Map();
  let pinch = null;
  function element(tag, className, text) {
    const item = document.createElement(tag);
    if (className) item.className = className;
    if (text !== undefined) item.textContent = text;
    return item;
  }
  function svg(tag, attributes) {
    const item = document.createElementNS("http://www.w3.org/2000/svg", tag);
    for (const [key, value] of Object.entries(attributes)) item.setAttribute(key, value);
    return item;
  }
  function button(text, action, className) {
    const item = element("button", className, text);
    item.type = "button";
    item.addEventListener("click", action);
    return item;
  }
  function announce(text) { $("announcement").textContent = text; }
  function viewport() { return { width: map.clientWidth, height: map.clientHeight }; }
  function localPoint(event) {
    const rect = map.getBoundingClientRect();
    return { x: event.clientX - rect.left, y: event.clientY - rect.top };
  }
  // Deliberate jumps (clicks, search, links) add a history entry. Keyboard
  // steps through the tree and expand/collapse replace it, so Back leaves
  // the page instead of retracing every arrow key.
  function writeBookmark(replace = false) {
    const hash = `${state.scope.id}/${state.selected}`;
    if (location.hash.slice(1) === hash) return;
    if (replace) history.replaceState(history.state, "", `#${hash}`);
    else location.hash = hash;
  }
  function applyTransform() {
    if (transformFrame) return;
    transformFrame = requestAnimationFrame(() => {
      transformFrame = 0;
      const view = state.view;
      if (!view) return;
      plane.style.transform = `translate(${view.x}px, ${view.y}px) scale(${view.scale})`;
      $("zoom-label").textContent = `${Math.round(view.scale * 100)}%`;
      const rect = mini.querySelector(".mini-view");
      if (rect) {
        rect.setAttribute("x", 6 - view.x / view.scale * miniScale);
        rect.setAttribute("y", 6 - view.y / view.scale * miniScale);
        rect.setAttribute("width", map.clientWidth / view.scale * miniScale);
        rect.setAttribute("height", map.clientHeight / view.scale * miniScale);
      }
    });
  }
  function fit() { state.view = model.fit(diagram.bounds, viewport()); applyTransform(); }
  function center(id, readable = false) {
    const position = diagram.nodes.find(p => p.node.id === id);
    if (!position) return;
    const scale = readable ? Math.max(.8, state.view?.scale || 1) : state.view?.scale || 1;
    state.view = { scale, x: map.clientWidth / 2 - (position.x + diagram.width / 2) * scale,
      y: map.clientHeight / 2 - (position.y + diagram.height / 2) * scale };
    applyTransform();
  }
  function renderMini() {
    mini.replaceChildren();
    miniScale = Math.min(178 / diagram.bounds.width, 113 / diagram.bounds.height);
    for (const p of diagram.nodes) mini.append(svg("rect", {
      x: 6 + p.x * miniScale, y: 6 + p.y * miniScale,
      width: diagram.width * miniScale, height: Math.max(2, diagram.height * miniScale),
      rx: 1, class: `mini-node${p.node.id === state.selected ? " selected" : ""}`
    }));
    mini.append(svg("rect", { class: "mini-view" }));
  }
  function renderTree(anchor) {
    const old = anchor && diagram?.nodes.find(p => p.node.id === anchor);
    diagram = model.layout(model.index(state.tree.nodes.get(state.focus)), state.expanded);
    nodes.replaceChildren(); edges.replaceChildren();
    edges.setAttribute("width", diagram.bounds.width);
    edges.setAttribute("height", diagram.bounds.height);
    const ancestors = new Set(model.path(state.tree, state.selected));
    for (const edge of diagram.edges) {
      const x = edge.from.x + diagram.width, y = edge.from.y + diagram.height / 2;
      const toX = edge.to.x, toY = edge.to.y + diagram.height / 2, mid = (x + toX) / 2;
      const grouped = edge.to.node.edge === "group";
      edges.append(svg("path", { d: `M ${x} ${y} C ${mid} ${y}, ${mid} ${toY}, ${toX} ${toY}`,
        class: `edge${grouped ? " group" : ""}${ancestors.has(edge.to.node.id) ? " active" : ""}` }));
    }
    for (const p of diagram.nodes) {
      const node = p.node, children = node.children || [];
      const card = element("div", "node");
      card.id = `map-node-${node.id}`;
      card.dataset.id = node.id;
      card.dataset.role = node.role;
      card.setAttribute("role", "treeitem");
      card.setAttribute("aria-label", `${node.title}. ${node.symbol}. ${node.role}`);
      card.setAttribute("aria-level", p.depth + 1);
      const siblings = state.tree.nodes.get(state.tree.parents.get(node.id))?.children || [node];
      card.setAttribute("aria-posinset", siblings.indexOf(node) + 1);
      card.setAttribute("aria-setsize", siblings.length);
      card.setAttribute("aria-selected", node.id === state.selected);
      card.tabIndex = node.id === state.selected ? 0 : -1;
      card.style.left = `${p.x}px`; card.style.top = `${p.y}px`;
      card.append(element("div", "node-kicker", node.role), element("h2", "", node.title),
        element("code", "", node.symbol), element("p", "", node.summary));
      if (children.length) {
        const expanded = state.expanded.has(node.id);
        card.setAttribute("aria-expanded", expanded);
        const toggle = button(expanded ? "−" : "+", event => { event.stopPropagation(); toggleBranch(node.id); }, "branch-button");
        toggle.setAttribute("aria-label", `${expanded ? "Collapse" : "Expand"} ${node.title}`);
        toggle.tabIndex = -1;
        card.append(toggle, element("span", "child-count", children.length));
      }
      card.addEventListener("click", () => select(state.scope.id, node.id, { focusKeyboard: true }));
      card.addEventListener("keydown", event => treeKey(event, node));
      nodes.append(card);
    }
    const now = anchor && diagram.nodes.find(p => p.node.id === anchor);
    if (old && now && state.view) {
      state.view.x += (old.x - now.x) * state.view.scale;
      state.view.y += (old.y - now.y) * state.view.scale;
    }
    if (!state.view) state.view = model.fit(diagram.bounds, viewport());
    $("node-count").textContent = `${diagram.nodes.length} / ${state.tree.nodes.size} nodes`;
    $("leave-branch").hidden = state.focus === state.scope.root.id;
    nodes.setAttribute("aria-label", state.scope.subtitle);
    renderMini(); applyTransform();
  }
  function toggleBranch(id) {
    const current = state.tree.nodes.get(id);
    if (!current.children?.length) return;
    if (state.expanded.has(id)) state.selected = model.collapse(state.tree, state.expanded, id, state.selected);
    else state.expanded.add(id);
    renderTree(id); renderDetails(); writeBookmark(true);
    $("map-node-" + state.selected)?.focus({ preventScroll: true });
    announce(`${current.title} ${state.expanded.has(id) ? "expanded" : "collapsed"}. ${diagram.nodes.length} visible nodes.`);
  }
  function section(title, items) {
    if (!items.length) return null;
    const part = element("section");
    part.append(element("h3", "", title), ...items);
    return part;
  }
  function jumpLink(label, scope, id, suffix = "→") {
    const item = button("", () => select(scope, id, { center: true, focusDetails: true }), "child-link");
    item.append(element("span", "", label), element("span", "", suffix));
    return item;
  }
  function renderDetails() {
    const node = state.tree.nodes.get(state.selected), detail = $("details");
    detail.replaceChildren();
    const badge = element("span", "role-badge", node.role === "UI" ? "UI · presents or owns controls" : node.role);
    badge.dataset.role = node.role;
    const heading = element("h2", "", node.title);
    heading.tabIndex = -1; // Receives focus after an inspector link replaces the details.
    detail.append(badge, heading, element("code", "symbol", node.symbol),
      element("p", "summary", node.summary), element("p", "detail-text", node.detail));
    const actions = element("div", "inspector-actions");
    actions.append(button("Locate on map", () => center(node.id, true)));
    if (node.children?.length) actions.append(button("Focus branch", () => {
      state.focus = node.id; state.expanded.add(node.id);
      renderTree(); fit(); announce(`Focused on ${node.title}. Use Whole map to return.`);
    }));
    detail.append(actions);
    const children = section(node.role === "Group" || state.scope.id === "modules" ? "Contains" : "Owned components / groups",
      (node.children || []).map(child => jumpLink(child.title, state.scope.id, child.id, child.role)));
    if (children) detail.append(children);
    if (node.borrows?.length) {
      const list = element("ul", "facts");
      list.append(...node.borrows.map(fact => element("li", "", fact)));
      detail.append(section("Explicitly borrows", [list]));
    }
    const refs = section(node.component ? "Direct dependencies" : "Related boundary",
      (node.references || []).map(ref => jumpLink(ref.label, ref.scope, ref.id)));
    if (refs) detail.append(refs);
    if (node.component) {
      // Mapped targets link to their nodes; other executables are listed as text.
      const target = node.component.target;
      const linked = [...state.tree.nodes.values()].flatMap(candidate => (candidate.component?.dependencies || [])
        .filter(dep => dep.target === target).map(dep => jumpLink(`${dep.visibility} · ${candidate.symbol}`, "modules", candidate.id)));
      const programs = (state.scope.programs || []).filter(program => program.links.includes(target)).map(program => {
        const row = element("p", "program", `PRIVATE · ${program.target}`);
        row.append(element("span", "", "executable"));
        return row;
      });
      const users = section("Used by", [...linked, ...programs]);
      if (users) detail.append(users);
      const external = section("External libraries", node.component.external.map(dep => element("p", "detail-text", `${dep.visibility} · ${dep.name}`)));
      if (external) detail.append(external);
      const ordered = section("Build after (not linking)", node.component.buildAfter.map(target => {
        const found = [...state.tree.nodes.values()].find(n => n.component?.target === target);
        return jumpLink(target, "modules", found.id);
      }));
      if (ordered) detail.append(ordered);
    }
    const sources = section("Go to source", node.sources.map(source => {
      const link = element("a", "source-link", source + " ↗");
      link.href = `../../${source}`; link.target = "_blank"; link.rel = "noopener";
      return link;
    }));
    detail.append(sources);
    const guide = element("a", "source-link", "Read the detailed guide ↗");
    guide.href = node.guide || state.scope.guide;
    detail.append(guide, element("p", "map-note", state.scope.id === "modules"
      ? "Dashed branches group build targets; dependencies are the separate lists above, not tree edges. Tests compare Direct dependencies and Used by with CMakeLists.txt. Used by leaves out test executables."
      : "Solid branches mean lifetime ownership; dashed branches are explicitly named groups. This is a curated source map, not live component status or a complete member listing."));
    const breadcrumbs = element("ol");
    $("breadcrumbs").replaceChildren(breadcrumbs);
    for (const [i, id] of model.path(state.tree, node.id).entries()) {
      const crumb = element("li");
      if (i) {
        const separator = element("span", "", "/");
        separator.setAttribute("aria-hidden", "true");
        crumb.append(separator);
      }
      const target = button(state.tree.nodes.get(id).title, () => select(state.scope.id, id, { center: true, focusKeyboard: true }));
      if (id === node.id) target.setAttribute("aria-current", "location");
      crumb.append(target);
      breadcrumbs.append(crumb);
    }
    breadcrumbs.scrollLeft = breadcrumbs.scrollWidth; // One-line path on short screens ends at the selection.
    $("scope-subtitle").textContent = state.scope.subtitle;
    for (const tab of $("scopes").children) tab.setAttribute("aria-pressed", tab.dataset.scope === state.scope.id);
  }
  function select(scopeId, id, options = {}) {
    const next = states.get(scopeId);
    if (!next?.tree.nodes.has(id)) return;
    const changedScope = state !== next;
    state = next;
    if (!model.path(state.tree, id).includes(state.focus)) state.focus = state.scope.root.id;
    state.selected = id;
    model.reveal(state.tree, state.expanded, id);
    renderTree(); renderDetails();
    if (options.center) center(id, true);
    else if (changedScope) applyTransform();
    // Re-rendering removes the control that was used; keep focus somewhere useful.
    if (options.focusKeyboard) $("map-node-" + id)?.focus({ preventScroll: true });
    else if (options.focusDetails) $("details").querySelector("h2").focus({ preventScroll: true });
    if (!options.fromHash) writeBookmark(options.replace);
    $("inspector").scrollTop = 0;
    announce(`${state.tree.nodes.get(id).title}. ${state.scope.subtitle}.`);
  }
  function overview() {
    state.focus = state.scope.root.id; state.expanded = new Set(state.scope.expanded);
    state.selected = state.scope.root.id;
    renderTree(); renderDetails(); fit(); writeBookmark();
  }
  function treeKey(event, node) {
    if (event.ctrlKey || event.metaKey || event.altKey) return;
    const visible = diagram.nodes.map(p => p.node.id), index = visible.indexOf(node.id);
    let destination;
    if (event.key === "ArrowRight") {
      if (node.children?.length && !state.expanded.has(node.id)) toggleBranch(node.id);
      else destination = node.children?.[0]?.id;
    } else if (event.key === "ArrowLeft") {
      if (node.children?.length && state.expanded.has(node.id)) toggleBranch(node.id);
      else destination = state.tree.parents.get(node.id);
    } else if (event.key === "ArrowDown") destination = visible[index + 1];
    else if (event.key === "ArrowUp") destination = visible[index - 1];
    else if (event.key === "Home") destination = visible[0];
    else if (event.key === "End") destination = visible.at(-1);
    else if (event.key === "Enter" || event.key === " ") toggleBranch(node.id);
    else return;
    event.preventDefault();
    if (destination) select(state.scope.id, destination, { center: true, focusKeyboard: true, replace: true });
  }
  function closeSearch() { results.hidden = true; }
  function searchResults() {
    results.replaceChildren();
    const matches = scopes.flatMap(scope => model.search(states.get(scope.id).tree, search.value).map(node => ({ scope, node })));
    if (!search.value.trim()) { closeSearch(); return; }
    results.hidden = false;
    results.append(element("p", "", matches.length ? `${matches.length} matches across all maps` : "No matches. Try a class name, responsibility or file."));
    for (const { scope, node } of matches) {
      const result = button("", () => {
        select(scope.id, node.id, { center: true, focusKeyboard: true });
        closeSearch();
      });
      result.append(element("span", "", node.title), element("small", "", `${scope.title} / ${node.symbol}`));
      results.append(result);
    }
    announce(`${matches.length} matching components.`);
  }
  search.addEventListener("input", searchResults);
  search.addEventListener("focus", searchResults);
  search.addEventListener("keydown", event => {
    if (event.key === "ArrowDown") { event.preventDefault(); results.querySelector("button")?.focus(); }
    if (event.key === "Enter") { event.preventDefault(); results.querySelector("button")?.click(); }
    if (event.key === "Escape") { search.value = ""; closeSearch(); search.blur(); }
  });
  results.addEventListener("keydown", event => {
    const buttons = [...results.querySelectorAll("button")], index = buttons.indexOf(document.activeElement);
    if (event.key === "ArrowDown") { event.preventDefault(); buttons[Math.min(index + 1, buttons.length - 1)]?.focus(); }
    if (event.key === "ArrowUp") { event.preventDefault(); if (index <= 0) search.focus(); else buttons[index - 1].focus(); }
    if (event.key === "Escape") { search.focus(); closeSearch(); }
  });
  document.addEventListener("pointerdown", event => { if (!event.target.closest(".search-wrap")) closeSearch(); });
  for (const scope of scopes) {
    const tab = button(scope.title, () => select(scope.id, states.get(scope.id).selected));
    tab.dataset.scope = scope.id; $("scopes").append(tab);
  }
  $("zoom-in").addEventListener("click", () => zoom(1.2, undefined, true));
  $("zoom-out").addEventListener("click", () => zoom(1 / 1.2, undefined, true));
  // Wheel and pinch zoom continuously and stay silent; a button or key
  // press is one deliberate step, so it announces the new level once.
  function zoom(factor, point = { x: map.clientWidth / 2, y: map.clientHeight / 2 }, speak = false) {
    state.view = model.zoom(state.view, factor, point); applyTransform();
    if (speak) announce(`Zoom ${Math.round(state.view.scale * 100)}%`);
  }
  $("fit").addEventListener("click", fit);
  $("overview").addEventListener("click", overview);
  $("expand-all").addEventListener("click", () => {
    for (const node of state.tree.nodes.values()) if (node.children?.length) state.expanded.add(node.id);
    renderTree(); fit(); announce("All branches expanded. Select a node or search to inspect it at a readable zoom.");
  });
  $("leave-branch").addEventListener("click", () => {
    state.focus = state.scope.root.id; renderTree(); fit();
    $("map-node-" + state.selected)?.focus({ preventScroll: true }); // This button is now hidden.
  });
  $("details-toggle").addEventListener("click", () => {
    const hidden = !$("inspector").hidden;
    $("inspector").hidden = hidden; document.body.classList.toggle("details-hidden", hidden);
    $("details-toggle").setAttribute("aria-expanded", !hidden);
  });
  map.addEventListener("wheel", event => {
    if (event.target.closest("button")) return;
    event.preventDefault();
    const delta = event.deltaY * (event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? map.clientHeight : 1);
    zoom(Math.exp(-Math.max(-240, Math.min(240, delta)) * .002), localPoint(event));
  }, { passive: false });
  map.addEventListener("pointerdown", event => {
    if (!pointers.size) suppressClick = false;
    if (event.button !== 0 || event.target.closest("button,.node")) return;
    const point = localPoint(event);
    pointers.set(event.pointerId, point);
    map.setPointerCapture(event.pointerId);
    if (pointers.size === 2) {
      const [a, b] = [...pointers.values()];
      pinch = { distance: Math.hypot(a.x - b.x, a.y - b.y), middle: { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 } };
      drag = null;
    } else drag = { pointer: event.pointerId, start: point, view: { ...state.view } };
    suppressClick = false; map.classList.add("panning");
  });
  map.addEventListener("pointermove", event => {
    if (!pointers.has(event.pointerId)) return;
    const point = localPoint(event); pointers.set(event.pointerId, point);
    if (pinch && pointers.size === 2) {
      const [a, b] = [...pointers.values()], distance = Math.hypot(a.x - b.x, a.y - b.y);
      const middle = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
      if (pinch.distance > 0) state.view = model.zoom(state.view, distance / pinch.distance, pinch.middle);
      state.view.x += middle.x - pinch.middle.x; state.view.y += middle.y - pinch.middle.y;
      pinch = { distance, middle }; suppressClick = true; applyTransform();
    } else if (drag?.pointer === event.pointerId) {
      const dx = point.x - drag.start.x, dy = point.y - drag.start.y;
      state.view = { ...drag.view, x: drag.view.x + dx, y: drag.view.y + dy };
      suppressClick ||= Math.hypot(dx, dy) > 3; applyTransform();
    }
  });
  function endPointer(event) {
    if (!pointers.delete(event.pointerId)) return;
    if (map.hasPointerCapture(event.pointerId)) map.releasePointerCapture(event.pointerId);
    pinch = null; drag = null;
    if (pointers.size === 1) {
      const [pointer, start] = [...pointers.entries()][0];
      drag = { pointer, start, view: { ...state.view } };
    } else map.classList.remove("panning");
  }
  for (const name of ["pointerup", "pointercancel", "lostpointercapture"]) map.addEventListener(name, endPointer);
  // A released pan must not accidentally activate a card under the pointer.
  map.addEventListener("click", event => {
    if (suppressClick) { event.preventDefault(); event.stopPropagation(); suppressClick = false; }
  }, true);
  document.addEventListener("keydown", event => {
    if (event.target.closest("input,textarea,select") || event.ctrlKey || event.metaKey || event.altKey) return;
    if (event.key === "/") { event.preventDefault(); search.focus(); }
    else if (event.key.toLowerCase() === "f") { event.preventDefault(); fit(); }
    else if (event.key === "Escape") { closeSearch(); }
    else if (event.target.closest("#map") && ["+", "=", "-"].includes(event.key)) {
      event.preventDefault(); zoom(event.key === "-" ? 1 / 1.2 : 1.2, undefined, true);
    }
  });
  new ResizeObserver(() => {
    const size = viewport();
    if (state.view) {
      state.view.x += (size.width - previousSize.width) / 2;
      state.view.y += (size.height - previousSize.height) / 2;
      applyTransform();
    }
    previousSize = size;
  }).observe(map);
  function readBookmark() {
    let value;
    try { value = decodeURIComponent(location.hash.slice(1)); } catch { value = ""; }
    if (!value) {
      select("editor", states.get("editor").scope.root.id, { fromHash: true });
      fit();
      return true;
    }
    const [scope, id] = value.split("/");
    if (!states.get(scope)?.tree.nodes.has(id)) return false;
    if (state.scope.id === scope && state.selected === id && diagram) return true;
    select(scope, id, { center: id !== states.get(scope).scope.root.id, fromHash: true });
    return true;
  }
  window.addEventListener("hashchange", () => { if (!readBookmark()) announce("Unknown map bookmark. Use search to find a component."); });
  if (!readBookmark()) { renderTree(); renderDetails(); fit(); }
})();
