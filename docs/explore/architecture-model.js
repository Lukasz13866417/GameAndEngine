/* Pure tree navigation and layout. Shared by the offline viewer and Node tests. */
(() => {
  "use strict";
  function index(root) {
    const nodes = new Map(), parents = new Map();
    function walk(node, parent) {
      if (nodes.has(node.id)) throw new Error(`Duplicate architecture node: ${node.id}`);
      nodes.set(node.id, node);
      if (parent) parents.set(node.id, parent.id);
      for (const child of node.children || []) walk(child, node);
    }
    walk(root);
    return { root, nodes, parents };
  }
  function path(tree, id) {
    if (!tree.nodes.has(id)) return [];
    const result = [id];
    while (tree.parents.has(result[0])) result.unshift(tree.parents.get(result[0]));
    return result;
  }
  function reveal(tree, expanded, id) {
    for (const parent of path(tree, id).slice(0, -1)) expanded.add(parent);
  }
  function collapse(tree, expanded, id, selected) {
    expanded.delete(id);
    return path(tree, selected).includes(id) ? id : selected;
  }
  function search(tree, query) {
    const words = query.toLowerCase().trim().split(/\s+/).filter(Boolean);
    if (!words.length) return [];
    return [...tree.nodes.values()].filter(node => {
      const text = [node.title, node.symbol, node.role, node.summary, node.detail,
        ...(node.sources || []), ...(node.borrows || [])].join(" ").toLowerCase();
      return words.every(word => text.includes(word));
    });
  }
  // Leaf slots reserve disjoint vertical bands; parents sit at their children's
  // midpoint. No measurements, force simulation or per-frame graph traversal.
  function layout(tree, expanded) {
    const width = 258, height = 102, column = 324, row = 126;
    const nodes = [], edges = [];
    let slot = 0;
    function place(node, depth) {
      const position = { node, x: depth * column, y: 0, depth };
      nodes.push(position);
      const children = expanded.has(node.id) ? node.children || [] : [];
      if (children.length) {
        const placed = children.map(child => place(child, depth + 1));
        position.y = (placed[0].y + placed.at(-1).y) / 2;
        for (const child of placed) edges.push({ from: position, to: child });
      } else position.y = slot++ * row;
      return position;
    }
    place(tree.root, 0);
    return { nodes, edges, width, height,
      bounds: { width: Math.max(...nodes.map(n => n.x)) + width, height: Math.max(...nodes.map(n => n.y)) + height } };
  }
  function fit(bounds, viewport, padding = 40) {
    const scale = Math.max(.01, Math.min(1, (viewport.width - padding * 2) / bounds.width,
      (viewport.height - padding * 2) / bounds.height));
    return { scale, x: (viewport.width - bounds.width * scale) / 2, y: (viewport.height - bounds.height * scale) / 2 };
  }
  function zoom(view, factor, point) {
    const scale = Math.max(.01, Math.min(1.8, view.scale * factor));
    const ratio = scale / view.scale;
    return { scale, x: point.x - (point.x - view.x) * ratio, y: point.y - (point.y - view.y) * ratio };
  }
  window.VNG_MAP_MODEL = { index, path, reveal, collapse, search, layout, fit, zoom };
})();
