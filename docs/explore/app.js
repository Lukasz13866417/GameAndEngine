(() => {
  "use strict";
  const page = window.VNG_GUIDE_PAGES.find(page => page.id === document.body.dataset.page);
  const routes = window.VNG_TOPIC_ROUTES;
  const allNodes = nodes => nodes.flatMap(node => [node, ...allNodes(node.children || [])]);
  const topics = new Map(allNodes(window.VNG_GUIDE).map(node => [node.id, node]));
  const topicHref = id => routes.get(id).id === page.id ? `#${id}` : `${routes.get(id).file}#${id}`;
  const navigation = document.querySelector("#navigation");
  navigation.replaceChildren();
  for (const [index, destination] of window.VNG_GUIDE_PAGES.entries()) {
    const nav = link("", destination.file);
    nav.append(element("span", "nav-number", String(index + 1).padStart(2, "0")), document.createTextNode(destination.title));
    if (destination.id === page.id) nav.setAttribute("aria-current", "page");
    navigation.append(nav);
  }
  function bookmark() {
    try { return decodeURIComponent(location.hash.slice(1)); } catch { return ""; }
  }
  function redirectTopic(id) {
    const destination = routes.get(id);
    if (!destination || destination.id === page.id) return false;
    // Preserve old index.html#topic URLs without server rules or broken history.
    location.replace(`${destination.file}#${encodeURIComponent(id)}`);
    return true;
  }
  const tree = document.querySelector("#tree");
  if (!tree) {
    const navigateStatic = () => {
      const id = bookmark();
      if (redirectTopic(id)) return;
      const target = document.getElementById(id);
      if (!target) return;
      const heading = target.querySelector("h1, h2") || target;
      heading.tabIndex = -1;
      requestAnimationFrame(() => { heading.focus({ preventScroll: true }); target.scrollIntoView({ block: "start" }); });
    };
    window.addEventListener("hashchange", navigateStatic);
    if (location.hash) navigateStatic();
    return;
  }
  const search = document.querySelector("#search");
  const clear = document.querySelector("#clear-search");
  const status = document.querySelector("#search-status");
  const collapse = document.querySelector("#collapse");
  const entries = new Map();
  const components = new Map(allNodes(window.VNG_GUIDE)
    .filter(node => node.component).map(node => [node.component.target, node]));
  const users = new Map([...components.keys()].map(target => [target, []]));
  for (const node of components.values()) {
    for (const dependency of node.component.dependencies) {
      users.get(dependency.target).push({ node, visibility: dependency.visibility });
    }
  }
  const initialTopic = page.roots[0];
  const savedOpen = new Set([initialTopic]);
  let searching = false;

  function element(tag, className, text) {
    const result = document.createElement(tag);
    if (className) result.className = className;
    if (text !== undefined) result.textContent = text;
    return result;
  }
  function link(label, href, className) {
    const anchor = element("a", className, label);
    anchor.href = href;
    return anchor;
  }
  function relationshipList(title, relationships, empty) {
    const section = element("section", "relationship-list");
    section.append(element("h3", "", title));
    if (!relationships.length) {
      section.append(element("p", "relationship-empty", empty));
      return section;
    }
    const list = element("ul");
    for (const relation of relationships) {
      const item = element("li");
      item.append(link(relation.node.title, topicHref(relation.node.id)),
        element("code", "relationship-target", relation.node.component.target),
        element("span", "relationship-scope", relation.visibility));
      list.append(item);
    }
    section.append(list);
    return section;
  }
  function renderDependencies(node) {
    const metadata = node.component;
    const dependents = users.get(metadata.target);
    const disclosure = element("details", "detail-links dependencies");
    disclosure.append(element("summary", "",
      `Dependencies & users · ${metadata.dependencies.length} direct · ${dependents.length} user${dependents.length === 1 ? "" : "s"}`));
    disclosure.append(element("p", "code-note",
      "Direct CMake links, not object ownership. Users cover this map; demos/tests outside it are not listed."));
    const columns = element("div", "dependency-columns");
    columns.append(relationshipList("Depends on", metadata.dependencies.map(dependency => ({
      node: components.get(dependency.target), visibility: dependency.visibility
    })), "No direct project-target links."));
    columns.append(relationshipList("Used by (mapped components)", dependents,
      "No mapped component links this target."));
    disclosure.append(columns);
    if (metadata.external.length) {
      const section = element("section", "relationship-list");
      section.append(element("h3", "", "External/library adapter links"));
      const list = element("ul");
      for (const dependency of metadata.external) {
        const item = element("li");
        item.append(element("code", "", dependency.name),
          element("span", "relationship-scope", dependency.visibility));
        list.append(item);
      }
      section.append(list);
      disclosure.append(section);
    }
    if (metadata.buildAfter.length) {
      const ordered = element("p", "code-note", "Build after (ordering only; not linked): ");
      for (const [index, target] of metadata.buildAfter.entries()) {
        if (index) ordered.append(document.createTextNode(", "));
        ordered.append(link(target, topicHref(components.get(target).id)));
      }
      disclosure.append(ordered);
    }
    disclosure.append(link("Read dependency labels", topicHref("code-components"), "permalink"));
    return disclosure;
  }
  function renderNode(node, ancestors = []) {
    const details = element("details", "node");
    details.id = node.id;
    details.open = savedOpen.has(node.id);
    const summary = element("summary");
    const arrow = element("span", "chevron", "›");
    arrow.setAttribute("aria-hidden", "true");
    const copy = element("span", "summary-copy");
    const title = element("span", "node-title", node.title);
    const subtitle = element("span", "node-summary", node.summary);
    copy.append(title, subtitle);
    const badge = element("span", "badge", node.kind);
    badge.dataset.kind = node.kind;
    summary.append(arrow, copy, badge);
    details.append(summary);
    const content = element("div", "node-content");
    if (node.symbol) {
      const identity = element("p", "component-identity");
      identity.append(element("code", "", node.symbol));
      content.append(identity);
    }
    if (node.component) {
      const identity = element("p", "component-identity");
      identity.append(element("code", "", node.component.target),
        document.createTextNode(` · ${node.component.type}`));
      content.append(identity);
    }
    for (const text of node.description || []) content.append(element("p", "", text));
    for (const section of node.sections || []) {
      const block = element("section", "component-section");
      block.append(element("h3", "", section.title));
      for (const paragraph of section.paragraphs) block.append(element("p", "", paragraph));
      if (section.code) {
        block.append(element("p", "code-note", section.exact
          ? "Source excerpt; surrounding setup and error handling may be outside this excerpt."
          : "Usage sketch; surrounding types, setup and error handling are omitted."));
        const pre = element("pre");
        pre.append(element("code", "", section.code));
        block.append(pre);
      }
      if (section.source) block.append(link(section.source, `../../${section.source}`, "section-source"));
      content.append(block);
    }
    if (node.steps) {
      const list = element("ol", "steps");
      for (const step of node.steps) list.append(element("li", "", step));
      content.append(list);
    }
    if (node.boundary) {
      const boundary = element("p", "boundary");
      boundary.append(element("strong", "", "Keep in mind. "), document.createTextNode(node.boundary));
      content.append(boundary);
    }
    if (node.ownership) {
      const ownership = element("dl", "ownership");
      for (const [key, label] of [["owns", "Owns"], ["borrows", "Borrows / receives"]]) {
        ownership.append(element("dt", "", label));
        const value = element("dd");
        const list = element("ul");
        for (const item of node.ownership[key]) list.append(element("li", "", item));
        value.append(node.ownership[key].length ? list : document.createTextNode(key === "owns" ? "No owned runtime state." : "No long-lived borrowed collaborator listed."));
        ownership.append(value);
      }
      content.append(ownership);
    }
    if (node.component) content.append(renderDependencies(node));
    if (node.api || node.links?.length) {
      const further = element("details", "detail-links");
      further.append(element("summary", "", node.api ? "Small API example & further reading" : "Further reading & source"));
      if (node.api) {
        further.append(element("p", "code-note", "Illustrative excerpt; assumes surrounding setup. Check expected results before using them. Follow the source link for complete code."));
        const pre = element("pre");
        pre.append(element("code", "", node.api));
        further.append(pre);
      }
      if (node.links?.length) {
        const list = element("ul");
        for (const target of node.links) {
          const item = element("li");
          item.append(link(target.label, target.href));
          list.append(item);
        }
        further.append(list);
      }
      content.append(further);
    }
    const related = element("div", "related");
    if (node.related?.length) content.append(related);
    content.append(link("Link to this topic", `#${node.id}`, "permalink"));
    if (node.children?.length) {
      const children = element("div", "children");
      for (const child of node.children) children.append(renderNode(child, [...ancestors, node.id]));
      content.append(children);
    }
    details.append(content);
    entries.set(node.id, { node, details, summary, title, subtitle, ancestors, related,
      searchable: [node.title, node.summary, ...(node.description || []), ...(node.steps || []), node.boundary || "", node.api || "",
        ...(node.links || []).flatMap(l => [l.label, l.href]),
        ...(node.sections || []).flatMap(s => [s.title, ...s.paragraphs, s.code || "", s.source || ""]),
        ...(node.ownership?.owns || []), ...(node.ownership?.borrows || []),
        node.component?.target || "", node.component?.type || "",
        ...(node.component?.dependencies || []).map(d => d.target),
        ...(node.component?.external || []).map(d => d.name)
      ].join(" ").toLocaleLowerCase() });
    // beforematch also opens ancestors when a browser supports find-in-page reveal.
    details.addEventListener("beforematch", () => openBranch(node.id));
    return details;
  }
  for (const id of page.roots) tree.append(renderNode(topics.get(id)));
  for (const entry of entries.values()) {
    if (!entry.node.related?.length) continue;
    entry.related.append(element("span", "", "Connects to"));
    for (const id of entry.node.related) entry.related.append(link(topics.get(id).title, topicHref(id)));
  }

  function highlight(target, text, term) {
    target.replaceChildren();
    let start = 0;
    let found;
    while (term && (found = text.toLocaleLowerCase().indexOf(term, start)) !== -1) {
      target.append(document.createTextNode(text.slice(start, found)), element("mark", "", text.slice(found, found + term.length)));
      start = found + term.length;
    }
    target.append(document.createTextNode(text.slice(start)));
  }
  function filter() {
    const query = search.value.trim().toLocaleLowerCase();
    if (query && !searching) {
      savedOpen.clear();
      for (const [id, entry] of entries) if (entry.details.open) savedOpen.add(id);
    }
    const wasSearching = searching;
    searching = Boolean(query);
    const matches = new Set();
    const visible = new Set();
    if (searching) {
      for (const [id, entry] of entries) {
        if (!entry.searchable.includes(query)) continue;
        matches.add(id);
        visible.add(id);
        for (const parent of entry.ancestors) visible.add(parent);
      }
    }
    for (const [id, entry] of entries) {
      entry.details.hidden = searching && !visible.has(id);
      if (searching) entry.details.open = visible.has(id);
      else if (wasSearching) entry.details.open = savedOpen.has(id);
      highlight(entry.title, entry.node.title, query);
      highlight(entry.subtitle, entry.node.summary, query);
    }
    clear.hidden = !search.value;
    collapse.disabled = searching;
    document.querySelector("#empty").hidden = !searching || matches.size > 0;
    status.textContent = searching
      ? `${matches.size} matching topic${matches.size === 1 ? "" : "s"}. Parent branches stay visible for context.`
      : "Click any block to expand it. Tab + Enter or Space works too.";
  }
  function openBranch(id) {
    const entry = entries.get(id);
    if (!entry) return;
    for (const parent of [...entry.ancestors, id]) entries.get(parent).details.open = true;
    return entry;
  }
  function navigate() {
    const id = bookmark();
    if (redirectTopic(id)) return;
    if (!entries.has(id)) return;
    search.value = "";
    filter();
    const entry = openBranch(id);
    // Wait until closed ancestors have layout before scrolling to the target.
    requestAnimationFrame(() => {
      entry.summary.focus({ preventScroll: true });
      entry.details.scrollIntoView({ block: "start" });
    });
  }
  document.addEventListener("click", (event) => {
    if (event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
    const anchor = event.target.closest("a[href^='#']");
    if (!anchor || !entries.has(anchor.hash.slice(1))) return;
    if (anchor.hash === location.hash) { event.preventDefault(); navigate(); }
  });
  search.addEventListener("input", filter);
  clear.addEventListener("click", () => { search.value = ""; filter(); search.focus(); });
  collapse.addEventListener("click", () => {
    for (const entry of entries.values()) entry.details.open = false;
    for (const detail of tree.querySelectorAll(".detail-links")) detail.open = false;
  });
  document.querySelector("#reset").addEventListener("click", () => {
    search.value = "";
    filter();
    for (const [id, entry] of entries) entry.details.open = id === initialTopic;
    for (const detail of tree.querySelectorAll(".detail-links")) detail.open = false;
    // Hash navigation keeps file:// support and browser history without storage.
    if (location.hash !== `#${initialTopic}`) location.hash = initialTopic;
    else navigate();
  });
  document.addEventListener("keydown", event => {
    if (event.ctrlKey || event.metaKey || event.altKey || event.isComposing) return;
    const editing = event.target.closest("input, textarea, [contenteditable='true']");
    if (event.key === "/" && !editing) { event.preventDefault(); search.focus(); }
    if (event.key === "Escape" && event.target === search) { search.value = ""; filter(); }
  });
  window.addEventListener("hashchange", navigate);
  filter();
  if (location.hash) navigate();
})();
