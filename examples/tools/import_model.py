#!/usr/bin/env python3
"""Convert a rigged, animated model from Blender's binary FBX into engine files.

    python3 examples/tools/import_model.py Soldier.fbx examples/assets/soldier --name soldier

Writes, into the output directory:
  NAME.vmesh       the bind-pose mesh with normals, colours and skin weights
                   (skin/bones/0-1 : u32x4, skin/weights/0-1 : f32x4)
  NAME.vrig        the armature and every animation clip (see docs/characters.md)
  NAME_pose.vmesh  the mesh deformed at one clip frame, for static scenes

Engine space: +Y up, the character faces +Z, one unit is one metre, feet at
Y = 0. Only deforming bones are kept. Each bone's pose is fitted to a rotation,
translation and uniform scale, the transforms the engine's rig supports; the
stretch and shear FBX composition can produce are dropped. Needs numpy.
"""
import argparse, collections, json, pathlib, struct, sys, zlib
import numpy as np

# --- Binary FBX (7.x) ---------------------------------------------------------

class Node:
    def __init__(self, name, props):
        self.name, self.props, self.children = name, props, []
    def find(self, name):
        return next((c for c in self.children if c.name == name), None)
    def all(self, name):
        return [c for c in self.children if c.name == name]
    def prop70(self):
        p70 = self.find("Properties70")
        return {p.props[0]: p.props[4:] for p in p70.all("P")} if p70 else {}

_ARRAYS = {"f": "<f4", "d": "<f8", "l": "<i8", "i": "<i4", "b": "?"}

def _property(data, pos):
    kind = chr(data[pos]); pos += 1
    scalar = {"Y": ("<h", 2), "C": ("?", 1), "I": ("<i", 4), "F": ("<f", 4), "D": ("<d", 8), "L": ("<q", 8)}
    if kind in scalar:
        fmt, size = scalar[kind]
        return struct.unpack_from(fmt, data, pos)[0], pos + size
    if kind in _ARRAYS:
        count, encoding, size = struct.unpack_from("<III", data, pos); pos += 12
        raw = data[pos:pos + size]; pos += size
        if encoding == 1: raw = zlib.decompress(raw)
        return np.frombuffer(raw, dtype=_ARRAYS[kind], count=count), pos
    if kind in "SR":
        size = struct.unpack_from("<I", data, pos)[0]; pos += 4
        raw = bytes(data[pos:pos + size])
        return (raw.decode("utf-8", "replace") if kind == "S" else raw), pos + size
    raise ValueError(f"unknown FBX property type {kind!r} at byte {pos}")

def _node(data, pos, wide):
    header = "<QQQB" if wide else "<IIIB"
    end, count, _, name_length = struct.unpack_from(header, data, pos)
    pos += struct.calcsize(header)
    if end == 0: return None, pos
    node = Node(data[pos:pos + name_length].decode("ascii"), []); pos += name_length
    for _ in range(count):
        value, pos = _property(data, pos); node.props.append(value)
    while pos < end:
        child, pos = _node(data, pos, wide)
        if child is None: break
        node.children.append(child)
    return node, end

def read_fbx(path):
    data = pathlib.Path(path).read_bytes()
    if not data.startswith(b"Kaydara FBX Binary  \0"):
        sys.exit(f"{path}: not a binary FBX (export from Blender with File > Export > FBX)")
    version = struct.unpack_from("<I", data, 23)[0]
    root, pos = Node("root", []), 27
    while pos < len(data):
        node, pos = _node(data, pos, version >= 7500)
        if node is None: break
        root.children.append(node)
    return root

# --- Scene evaluation -----------------------------------------------------------

KTIME = 46186158000  # FBX time units per second

def euler_xyz(degrees):
    x, y, z = np.radians(degrees)
    rx = np.array([[1, 0, 0], [0, np.cos(x), -np.sin(x)], [0, np.sin(x), np.cos(x)]])
    ry = np.array([[np.cos(y), 0, np.sin(y)], [0, 1, 0], [-np.sin(y), 0, np.cos(y)]])
    rz = np.array([[np.cos(z), -np.sin(z), 0], [np.sin(z), np.cos(z), 0], [0, 0, 1]])
    return rz @ ry @ rx  # eEulerXYZ: X is applied first

def trs_matrix(t, r, s):
    m = np.eye(4); m[:3, :3] = euler_xyz(r) * np.asarray(s)[None, :]; m[:3, 3] = t
    return m

def display_name(node):
    return node.props[1].split("\x00\x01")[0]

class FbxScene:
    """Models, meshes, skins and animation of a Blender FBX, evaluated as the
    FBX SDK composes transforms (every model here inherits RSrs)."""

    def __init__(self, root):
        objects = root.find("Objects")
        self.objects = objects
        self.byid = {o.props[0]: o for o in objects.children}
        self.connections = [c.props for c in root.find("Connections").all("C")]
        self.models = {o.props[0]: o for o in objects.all("Model")}
        self.parent = {c[1]: c[2] for c in self.connections
                       if c[1] in self.models and (c[2] in self.models or c[2] == 0)}
        self.children = collections.defaultdict(list)
        for c in self.connections: self.children[c[2]].append((c[1], c[3] if len(c) > 3 else None))
        self.stored = {i: self.stored_local(m) for i, m in self.models.items()}
        self.bind = {}
        for pose in objects.all("Pose"):
            for node in pose.all("PoseNode"):
                self.bind.setdefault(node.find("Node").props[0], node.find("Matrix").props[0].reshape(4, 4).T.copy())
        self.meshes = [self.load_mesh(g) for g in objects.all("Geometry") if g.props[2] == "Mesh"]
        self.clips = self.load_clips()

    @staticmethod
    def stored_local(model):
        p = model.prop70()
        return (np.array(p.get("Lcl Translation", [0, 0, 0]), float), np.array(p.get("Lcl Rotation", [0, 0, 0]), float),
                np.array(p.get("Lcl Scaling", [1, 1, 1]), float))

    def globals(self, locals_):
        out = {}
        def resolve(i):
            if i not in out:
                p = self.parent.get(i)
                out[i] = (resolve(p) if p in self.models else np.eye(4)) @ trs_matrix(*locals_[i])
            return out[i]
        for i in self.models: resolve(i)
        return out

    def bind_globals(self):
        """World matrices at bind time: the bind pose, else the stored transforms."""
        return {**self.globals(self.stored), **self.bind}

    def material_color(self, material):
        p = material.prop70()
        color = np.array(p.get("DiffuseColor", [0.8, 0.8, 0.8]), float)
        return None if color.max() <= 1e-6 else color  # black means the colour came from nodes FBX cannot hold

    def load_mesh(self, geometry):
        model_id = next(c[2] for c in self.connections if c[1] == geometry.props[0] and c[2] in self.models)
        positions = geometry.find("Vertices").props[0].reshape(-1, 3).astype(float)
        index = geometry.find("PolygonVertexIndex").props[0]
        ends = np.flatnonzero(index < 0)
        if len(ends) * 3 != len(index):
            sys.exit(f"{display_name(self.models[model_id])}: only triangles are supported (triangulate before export)")
        corners = np.where(index < 0, ~index, index)
        normals = self.layer(geometry, "LayerElementNormal", "Normals", 3, len(corners), corners)
        materials = [self.byid[c] for c, _ in self.children[model_id] if c in self.byid and self.byid[c].name == "Material"]
        layer = geometry.find("LayerElementMaterial")
        per_face = layer.find("Materials").props[0].astype(int) if layer else np.zeros(1, int)
        if layer is None or layer.find("MappingInformationType").props[0] == "AllSame":
            per_face = np.full(len(ends), per_face[0])
        skin = next((self.byid[c] for c, _ in self.children[geometry.props[0]]
                     if c in self.byid and self.byid[c].name == "Deformer"), None)
        influences = [[] for _ in range(len(positions))]
        links = {}
        if skin is not None:
            for cluster_id, _ in self.children[skin.props[0]]:
                cluster = self.byid[cluster_id]; indexes = cluster.find("Indexes")
                if indexes is None or not len(indexes.props[0]): continue
                bone = next(c for c, _ in self.children[cluster_id] if c in self.models)
                # Blender writes Transform = inverse(bone at bind) * mesh at bind.
                links[bone] = cluster.find("Transform").props[0].reshape(4, 4).T.copy()
                for v, w in zip(indexes.props[0], cluster.find("Weights").props[0]):
                    if w > 0: influences[v].append((bone, float(w)))
        return dict(name=display_name(self.models[model_id]), model=model_id, positions=positions,
                    corners=corners, normals=normals, face_material=per_face, materials=materials,
                    influences=influences, links=links)

    @staticmethod
    def layer(geometry, name, values_name, width, corner_count, corners):
        layer = geometry.find(name)
        if layer is None: return None
        values = layer.find(values_name).props[0].reshape(-1, width).astype(float)
        mapping = layer.find("MappingInformationType").props[0]
        reference = layer.find("ReferenceInformationType").props[0]
        if reference == "IndexToDirect":
            values = values[layer.find(values_name + "Index" if name != "LayerElementNormal" else "NormalsIndex").props[0]]
        if mapping == "ByPolygonVertex": return values
        if mapping in ("ByVertice", "ByVertex"): return values[corners]
        sys.exit(f"unsupported {name} mapping {mapping}")

    def load_clips(self):
        clips = []
        for stack in self.objects.all("AnimationStack"):
            layers = [c for c, _ in self.children[stack.props[0]] if self.byid[c].name == "AnimationLayer"]
            channels = collections.defaultdict(dict)
            times = set()
            for layer in layers:
                for node_id, _ in self.children[layer]:
                    target = next(((c[2], c[3]) for c in self.connections
                                   if c[1] == node_id and c[2] in self.models and len(c) > 3), None)
                    if target is None or target[1] not in ("Lcl Translation", "Lcl Rotation", "Lcl Scaling"): continue
                    for curve_id, prop in self.children[node_id]:
                        curve = self.byid[curve_id]
                        key_times = curve.find("KeyTime").props[0] / KTIME
                        times.update(np.round(key_times, 6).tolist())
                        channels[target[0]][(target[1], "XYZ".index(prop[-1]))] = (key_times, curve.find("KeyValueFloat").props[0].astype(float))
            clips.append(dict(name=display_name(stack).split("|")[-1], times=np.array(sorted(times)), channels=channels))
        # Blender writes every action once per stack; identical copies are one clip.
        unique = []
        for clip in clips:
            if not any(self.same_clip(clip, other) for other in unique): unique.append(clip)
        return unique

    @staticmethod
    def same_clip(a, b):
        if len(a["times"]) != len(b["times"]) or a["channels"].keys() != b["channels"].keys(): return False
        return all(np.allclose(a["channels"][m][k][1], b["channels"][m][k][1])
                   for m in a["channels"] for k in a["channels"][m])

    def locals_at(self, clip, t):
        locals_ = {i: tuple(v.copy() for v in value) for i, value in self.stored.items()}
        for model, channels in clip["channels"].items():
            for (prop, axis), (times, values) in channels.items():
                slot = {"Lcl Translation": 0, "Lcl Rotation": 1, "Lcl Scaling": 2}[prop]
                locals_[model][slot][axis] = np.interp(t, times, values)
        return locals_

# --- Engine space --------------------------------------------------------------

def similarity(m):
    """Nearest rotation, uniform scale and translation to an affine matrix."""
    u, _, vt = np.linalg.svd(m[:3, :3]); r = u @ vt
    if np.linalg.det(r) < 0: u[:, -1] *= -1; r = u @ vt
    out = np.eye(4); out[:3, :3] = r * np.cbrt(abs(np.linalg.det(m[:3, :3]))); out[:3, 3] = m[:3, 3]
    return out

def quaternion(r):
    """Unit quaternion (x, y, z, w) of a rotation matrix."""
    t = np.trace(r)
    if t > 0:
        s = 2 * np.sqrt(t + 1); q = [(r[2, 1] - r[1, 2]) / s, (r[0, 2] - r[2, 0]) / s, (r[1, 0] - r[0, 1]) / s, s / 4]
    elif r[0, 0] > r[1, 1] and r[0, 0] > r[2, 2]:
        s = 2 * np.sqrt(1 + r[0, 0] - r[1, 1] - r[2, 2]); q = [s / 4, (r[0, 1] + r[1, 0]) / s, (r[0, 2] + r[2, 0]) / s, (r[2, 1] - r[1, 2]) / s]
    elif r[1, 1] > r[2, 2]:
        s = 2 * np.sqrt(1 + r[1, 1] - r[0, 0] - r[2, 2]); q = [(r[0, 1] + r[1, 0]) / s, s / 4, (r[1, 2] + r[2, 1]) / s, (r[0, 2] - r[2, 0]) / s]
    else:
        s = 2 * np.sqrt(1 + r[2, 2] - r[0, 0] - r[1, 1]); q = [(r[0, 2] + r[2, 0]) / s, (r[1, 2] + r[2, 1]) / s, s / 4, (r[1, 0] - r[0, 1]) / s]
    q = np.array(q); return q / np.linalg.norm(q)

def rotation_matrix(q):
    """Rotation matrix of a unit quaternion (x, y, z, w)."""
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])

def halfway(upper, lower):
    """A joint's half-angle frame: the rotation halfway from the upper bone's
    to the lower bone's, at the lower bone's head; the bones' mean scale."""
    su, sl = np.cbrt(np.linalg.det(upper[:3, :3])), np.cbrt(np.linalg.det(lower[:3, :3]))
    qu, ql = quaternion(upper[:3, :3] / su), quaternion(lower[:3, :3] / sl)
    if qu @ ql < 0: ql = -ql
    out = np.eye(4); out[:3, :3] = rotation_matrix((qu + ql) / np.linalg.norm(qu + ql)) * (su + sl) / 2
    out[:3, 3] = lower[:3, 3]
    return out

def decompose(m):
    """(translation, quaternion, uniform scale) of a similarity matrix."""
    scale = np.cbrt(np.linalg.det(m[:3, :3]))
    return m[:3, 3].copy(), quaternion(m[:3, :3] / scale), float(scale)

class Character:
    """The FBX scene in engine space: deforming bones only, similarity poses."""

    def __init__(self, scene, height):
        self.scene = scene
        bind = scene.bind_globals()
        deforming = {b for m in scene.meshes for b in m["links"]}
        parent = {b: self.deforming_parent(b, deforming) for b in deforming}
        depth = lambda b: 0 if parent[b] is None else 1 + depth(parent[b])
        # Parents before children (the armature builder's rule), then by name.
        self.deforming = sorted(deforming, key=lambda b: (depth(b), display_name(scene.models[b])))
        # Blender Z-up to engine Y-up, turned to face +Z, scaled to the height.
        points = np.concatenate([self.bind_points(m, bind) for m in scene.meshes])
        convert = np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0]], float)
        turn = np.diag([-1.0, 1.0, -1.0])
        up = points @ convert.T
        self.scale = height / (up[:, 1].max() - up[:, 1].min())
        self.linear = turn @ convert
        self.offset = np.array([0.0, -up[:, 1].min() * self.scale, 0.0])
        self.names = ["root"] + [display_name(scene.models[b]) for b in self.deforming]
        index = {b: k + 1 for k, b in enumerate(self.deforming)}
        self.parents = [-1] + [index.get(parent[b], 0) for b in self.deforming]
        self.bone_index = index
        self.halves = []
        self.rest = self.bone_worlds(bind)

    def deforming_parent(self, model, deforming):
        """The nearest deforming ancestor, or None. Rigify hangs many DEF bones
        (shoulders, breasts, the face) from mechanism bones; an ORG- ancestor
        stands for the DEF- bone that copies it, so those keep their place in
        the body instead of hanging from the root."""
        models = self.scene.models
        twins = {display_name(models[b]): b for b in deforming}
        ancestors = []; up = self.scene.parent.get(model)
        while up in models: ancestors.append(up); up = self.scene.parent.get(up)
        for up in ancestors:
            if up in deforming: return up
            name = display_name(models[up])
            twin = twins.get("DEF-" + name[4:]) if name.startswith("ORG-") else None
            # The twin must sit above this bone, or the tree would loop.
            if twin is not None and twin != model and model not in self.ancestry(twin):
                return twin
        return None

    def ancestry(self, model):
        out = []; model = self.scene.parent.get(model)
        while model in self.scene.models: out.append(model); model = self.scene.parent.get(model)
        return out

    @staticmethod
    def bind_points(mesh, bind):
        homo = np.c_[mesh["positions"], np.ones(len(mesh["positions"]))]
        return (homo @ bind[mesh["model"]].T)[:, :3]

    def point(self, p):
        return (p @ self.linear.T) * self.scale + self.offset

    def world(self, m):
        """An FBX world matrix as a unit-scale engine bone world, with the character's placement."""
        out = np.eye(4)
        out[:3, :3] = self.linear @ m[:3, :3]
        out[:3, 3] = self.point(m[:3, 3])
        return similarity(out)

    def bone_worlds(self, globals_):
        worlds = [np.eye(4)]
        for b in self.deforming: worlds.append(self.world(globals_[b]))
        return self.with_halves(worlds)

    # Joints that get a half-angle helper: (name, upper bone, lower bone).
    HALF_JOINTS = (("elbow", "DEF-upper_arm.{}.001", "DEF-forearm.{}"), ("knee", "DEF-thigh.{}.001", "DEF-shin.{}"))

    def add_half_joints(self):
        """Volume helpers: at each elbow and knee, a bone that turns half as far
        as the joint does. Linear blend skinning averages the two bones'
        matrices across a joint, which shrinks a bent elbow to half its
        thickness; skin shared by the two bones moves onto the helper
        (share_halves), which turns rigidly and keeps it."""
        for joint, upper, lower in self.HALF_JOINTS:
            for side in "LR":
                u, l = upper.format(side), lower.format(side)
                if u not in self.names or l not in self.names: continue
                self.names.append(f"HALF-{joint}.{side}"); self.parents.append(self.names.index(u))
                self.halves.append((len(self.names) - 1, self.names.index(u), self.names.index(l)))
        self.rest = self.with_halves(self.rest)

    def with_halves(self, worlds):
        """Bone worlds with the half-angle helpers set from their joints' bones."""
        worlds = list(worlds[:len(self.deforming) + 1])
        for _, u, l in self.halves: worlds.append(halfway(worlds[u], worlds[l]))
        return worlds

    def share_halves(self, skin):
        """Skin [(bone, weight)] with each joint's shared weight on its helper:
        a vertex weighted a to the upper bone and b to the lower gives
        min(a, b) of each to the helper, so a 50/50 vertex turns rigidly by
        half the joint's angle."""
        skin = dict(skin)
        for h, u, l in self.halves:
            shared = min(skin.get(u, 0.0), skin.get(l, 0.0))
            if shared < 0.01: continue
            skin[u] -= shared; skin[l] -= shared; skin[h] = skin.get(h, 0.0) + 2 * shared
        return [(b, w) for b, w in sorted(skin.items(), key=lambda x: -x[1]) if w > 1e-6]

    def locals(self, worlds):
        return [decompose(np.linalg.inv(worlds[p]) @ w) if p >= 0 else decompose(w)
                for w, p in zip(worlds, self.parents)]

    def skinning(self, worlds):
        """Per deforming bone: pose world times inverse rest world (engine convention)."""
        return [w @ np.linalg.inv(r) for w, r in zip(worlds, self.rest)]

# --- Output ---------------------------------------------------------------------

def f32(x):
    return str(np.float32(x)).removesuffix(".0") if np.isfinite(x) else sys.exit("non-finite value")

def vector(values):
    return "[" + ", ".join(f32(v) for v in values) + "]"

def quote(text):
    return json.dumps(text, ensure_ascii=False)

def build_mesh(character, colors, pose_worlds=None, max_influences=8):
    """Vertices split wherever position, normal or colour differ; skin per vertex."""
    scene = character.scene; bind = scene.bind_globals()
    rows, faces, lookup = [], [], {}
    skinning = character.skinning(pose_worlds) if pose_worlds is not None else None
    for mesh in scene.meshes:
        bind_points = character.point(character.bind_points(mesh, bind))
        normal_matrix = np.linalg.inv(bind[mesh["model"]][:3, :3]).T
        for face, material_index in enumerate(mesh["face_material"]):
            color = colors(mesh, mesh["materials"][material_index] if mesh["materials"] else None)
            triangle = []
            for c in range(3):
                corner = face * 3 + c; vertex = mesh["corners"][corner]
                n = character.linear @ (normal_matrix @ mesh["normals"][corner])
                n /= np.linalg.norm(n)
                influences = sorted(mesh["influences"][vertex], key=lambda x: -x[1])[:max_influences]
                total = sum(w for _, w in influences)
                skin = [(character.bone_index[b], w / total) for b, w in influences] if total > 1e-4 else [(0, 1.0)]
                skin = character.share_halves(skin)
                p = bind_points[vertex]
                if skinning is not None:
                    blend = sum(w * skinning[b] for b, w in skin)
                    p = (blend @ np.r_[p, 1])[:3]
                    n = np.linalg.inv(blend[:3, :3]).T @ n; n /= np.linalg.norm(n)
                key = (tuple(np.round(p, 6)), tuple(np.round(n, 4)), color)
                if key not in lookup:
                    lookup[key] = len(rows); rows.append((p, n, color, skin))
                triangle.append(lookup[key])
            faces.append(triangle)
    return rows, faces

def write_mesh(path, name, source, rows, faces, rig=None, max_influences=8):
    lines = ["vmesh 1.0", "info {", f"    name = {quote(name)};", f"    source/file = {quote(source)};",
             '    source/tool = "examples/tools/import_model.py";']
    if rig: lines.append(f"    skin/rig = {quote(rig)};")
    lines += ["}", f"vertices {len(rows)} {{", "    fields {", "        position : f32x3;", "        normal : f32x3;",
              "        color/0 : f32x4;"]
    groups = (max_influences + 3) // 4 if rig else 0
    for g in range(groups): lines += [f"        skin/bones/{g} : u32x4;", f"        skin/weights/{g} : f32x4;"]
    lines += ["    }", "    data {"]
    for p, n, color, skin in rows:
        row = f"        {vector(p)} {vector(n)} {vector(color)}"
        padded = skin + [(0, 0.0)] * (groups * 4 - len(skin))
        for g in range(groups):
            part = padded[g * 4:g * 4 + 4]
            row += " [" + ", ".join(str(b) for b, _ in part) + "] " + vector([w for _, w in part])
        lines.append(row + ";")
    lines += ["    }", "}", f"faces {len(faces)} {{"] + [f"    [{a}, {b}, {c}];" for a, b, c in faces] + ["}"]
    pathlib.Path(path).write_text("\n".join(lines) + "\n")

def write_rig(path, name, source, character, clips):
    lines = ["vrig 1.0", "rig = 1;", f"name = {quote(name)};", f"source = {quote(source)};", "bones = ["]
    for bone, (parent, (t, q, s)) in enumerate(zip(character.parents, character.locals(character.rest))):
        parent_name = character.names[parent] if parent >= 0 else ""
        lines.append(f"    {{ name = {quote(character.names[bone])}; parent = {quote(parent_name)}; "
                     f"translation = {vector(t)}; rotation = {vector(q)}; scale = {f32(s)}; }},")
    lines += ["];", "clips = ["]
    for clip in clips:
        lines += ["    {", f"        name = {quote(clip['name'])};", f"        fps = {f32(clip['fps'])};",
                  f"        frames = {len(clip['frames'])};", f"        loop = {'true' if clip['loop'] else 'false'};",
                  f"        speed = {f32(clip['speed'])};", "        tracks = ["]
        for bone in range(len(character.names)):
            frames = [frame[bone] for frame in clip["frames"]]
            lines.append(f"            {{ bone = {quote(character.names[bone])}; "
                         f"translation = {vector(np.concatenate([t for t, _, _ in frames]))}; "
                         f"rotation = {vector(np.concatenate([q for _, q, _ in frames]))}; "
                         f"scale = {vector([s for _, _, s in frames])}; }},")
        lines += ["        ];", "    },"]
    lines.append("];")
    pathlib.Path(path).write_text("\n".join(lines) + "\n")

def frames_from_worlds(character, worlds):
    """Per-frame local transforms, quaternions kept on one hemisphere for blending."""
    frames, previous = [], None
    for w in worlds:
        local = character.locals(w)
        if previous is not None:
            local = [(t_, -q if q @ pq < 0 else q, s) for (t_, q, s), (_, pq, _) in zip(local, previous)]
        frames.append(local); previous = local
    return frames

def sample_clip(scene, character, clip):
    worlds = [character.bone_worlds(scene.globals(scene.locals_at(clip, t))) for t in clip["times"]]
    return frames_from_worlds(character, worlds), worlds

def fill_unweighted(scene):
    """Vertices Blender leaves unweighted stay put there; here they take their
    nearest weighted neighbour's weights, so they move with what they touch."""
    filled = 0
    for mesh in scene.meshes:
        weighted = np.array([v for v, inf in enumerate(mesh["influences"]) if inf])
        if not len(weighted): continue
        for v, inf in enumerate(mesh["influences"]):
            if inf: continue
            nearest = weighted[np.argmin(np.linalg.norm(mesh["positions"][weighted] - mesh["positions"][v], axis=1))]
            mesh["influences"][v] = list(mesh["influences"][nearest]); filled += 1
    return filled

def hand_grips(scene, character, first_frame, dip=0.0, lower=0.0):
    """Grips for a mesh held wholly by one hand (the soldier's rifle): that
    hand's fingers close on it, and the other hand takes its far hole the way
    the holding hand takes the near one (or, without two holes, supports it
    from below). The held mesh's far end dips `dip` degrees about its rear,
    and the whole of it sits `lower` metres lower."""
    import grip
    bind = scene.bind_globals()
    hands_mesh = next((m for m in scene.meshes if m["name"].lower().startswith("hand")), None)
    if hands_mesh is None: return {}
    points = character.point(Character.bind_points(hands_mesh, bind))
    bones = np.array([character.bone_index[max(inf, key=lambda x: x[1])[0]] for inf in hands_mesh["influences"]])
    for mesh in scene.meshes:
        owners = {b for inf in mesh["influences"] for b, _ in inf}
        if len(owners) != 1: continue
        holder = display_name(scene.models[next(iter(owners))])
        if not holder.startswith("DEF-hand."): continue
        side = holder[-1]; other = "R" if side == "L" else "L"
        held = character.point(Character.bind_points(mesh, bind))
        triangles = held[mesh["corners"].reshape(-1, 3)]
        holding, support = grip.Hand(character, side, points, bones), grip.Hand(character, other, points, bones)
        h = character.bone_index[next(iter(owners))]
        holding_world = first_frame[h] @ np.linalg.inv(character.rest[h])
        placement = grip.hole_placement(holding, support, held, triangles, holding_world)
        if placement is None: placement = grip.support_placement(support, held, triangles, holding_world)
        on_support = placement @ np.linalg.inv(character.rest[support.hand])
        frame = grip.held_frame(held, character.rest[h][:3, 3], holding_world)
        rear = frame[:3, 3] + frame[:3, 0] * ((held - frame[:3, 3]) @ frame[:3, 0]).min()
        carry = np.eye(4); carry[:3, :3] = grip._rotation(frame[:3, 2], -dip)
        carry[:3, 3] = rear - carry[:3, :3] @ rear - frame[:3, 1] * lower
        # Both thumbs point along it, toward its far end (the muzzle), drooping
        # toward the floor of the clip's first frame where they can.
        forward = grip.long_axis(held, character.rest[h][:3, 3])
        down = -np.linalg.inv(first_frame[h] @ np.linalg.inv(character.rest[h]))[:3, 1]
        holding.aim_thumb(triangles, np.eye(4), forward, down)
        support.aim_thumb(triangles, on_support, forward, down)
        return {side: dict(hand=holding, curls=holding.close(triangles), support=None, carry=carry),
                other: dict(hand=support, curls=support.close(triangles, on_support), support=placement)}
    return {}

def held_out_of_body(scene, character, hands, worlds):
    """If what a hand holds sinks into the character's torso or head (the
    soldier's authored aim pressed the rifle's butt into his chin), pull it
    along its length until it clears: the holding hand's carry. Returns the
    distance."""
    import grip
    holding = next(((side, g) for side, g in hands.items() if g.get("support") is None), None)
    if holding is None: return 0.0
    side, g = holding
    bind = scene.bind_globals(); hand = character.names.index(f"DEF-hand.{side}")
    skin = character.skinning(worlds)
    def posed(mesh):
        points = character.point(Character.bind_points(mesh, bind)); out = np.zeros_like(points)
        for v, inf in enumerate(mesh["influences"]):
            top = sorted(inf, key=lambda x: -x[1])[:8]; total = sum(w for _, w in top)
            for b, w in top: out[v] += w / total * (skin[character.bone_index[b]] @ np.r_[points[v], 1.0])[:3]
        return out
    held = next(m for m in scene.meshes if {b for inf in m["influences"] for b, _ in inf} == {character.deforming[hand - 1]})
    # The torso and head only: arms reach round what they hold.
    limb = ("upper_arm", "forearm", "hand", "palm", "f_", "thumb")
    def trunk(mesh):
        own = np.array([not any(k in display_name(scene.models[max(inf, key=lambda x: x[1])[0]]) for k in limb)
                        for inf in mesh["influences"]])
        corners = mesh["corners"].reshape(-1, 3)
        return posed(mesh)[corners[own[corners].all(1)]]
    triangles = np.concatenate([trunk(m) for m in scene.meshes if m is not held])
    bind_points = character.point(Character.bind_points(held, bind))
    forward = grip.long_axis(bind_points, character.rest[hand][:3, 3])
    distance = grip.clear_of(posed(held), triangles, skin[hand][:3, :3] @ forward)
    if not distance: return 0.0
    pull = np.eye(4); pull[:3, 3] = forward * distance
    g["carry"] = pull @ g.get("carry", np.eye(4))
    return distance

def sole_points(character, rows):
    """Rest-pose points on each foot's sole (vertices mostly bound to that side's foot or toe)."""
    points = np.array([p for p, _, _, _ in rows])
    dominant = np.array([max(skin, key=lambda x: x[1])[0] for _, _, _, skin in rows])
    out = {}
    for side in ("L", "R"):
        foot = np.isin(dominant, [i for i, n in enumerate(character.names) if n.endswith("." + side) and ("foot" in n or "toe" in n)])
        out[side] = points[foot]
    return out

def skin_rows(rows, skinning):
    """Posed positions of mesh rows (vectorized linear blend skinning)."""
    points = np.array([np.r_[p, 1.0] for p, _, _, _ in rows])
    matrices = np.array(skinning)
    out = np.zeros((len(rows), 3))
    for k in range(max(len(skin) for _, _, _, skin in rows)):
        bones = np.array([skin[k][0] if k < len(skin) else 0 for _, _, _, skin in rows])
        weights = np.array([skin[k][1] if k < len(skin) else 0.0 for _, _, _, skin in rows])
        out += weights[:, None] * np.einsum("nij,nj->ni", matrices[bones], points)[:, :3]
    return out

def walking_speed(character, rows, worlds, fps):
    """Forward speed of the ground under planted feet, in metres per second.

    Each frame, the lowest vertex of each foot (vertices mostly bound to that
    side's foot or toe bones) is the one on the ground while the sole is within
    5 cm of its lowest point. Following that vertex to the next frame gives the
    ground's speed; the median over both feet ignores the lift-off frames.
    Scrolling the ground this fast leaves the least slip."""
    dominant = np.array([max(skin, key=lambda x: x[1])[0] for _, _, _, skin in rows])
    posed = np.array([skin_rows(rows, character.skinning(w)) for w in worlds])
    speeds = []
    for side in (".L", ".R"):
        foot = np.flatnonzero(np.isin(dominant, [i for i, n in enumerate(character.names)
                                                 if n.endswith(side) and ("foot" in n or "toe" in n)]))
        if not len(foot): continue
        floor = posed[:, foot, 1].min()
        for f in range(len(worlds)):
            heights = posed[f, foot, 1]
            if heights.min() > floor + 0.05: continue
            planted = foot[np.argmin(heights)]
            speeds.append(-(posed[(f + 1) % len(worlds), planted, 2] - posed[f, planted, 2]) * fps)
    return float(np.median(speeds)) if speeds else 0.0

def report_fidelity(scene, character, clip_worlds, max_influences=8):
    """Print what the engine's form of the rig cannot reproduce: measured, not hidden."""
    unweighted = sum(1 for m in scene.meshes for inf in m["influences"] if not inf)
    over = [len(inf) for m in scene.meshes for inf in m["influences"] if len(inf) > max_influences]
    if unweighted: print(f"  {unweighted} vertices have no skin weights; they follow the root bone")
    if over: print(f"  {len(over)} vertices had up to {max(over)} influences; the {max_influences} largest are kept")
    if not scene.clips: return
    clip, bind = scene.clips[0], scene.bind_globals()
    meshes = []
    for m in scene.meshes:
        weighted = [v for v, inf in enumerate(m["influences"]) if inf]
        if not weighted: continue
        width = max(len(m["influences"][v]) for v in weighted)
        order = sorted(m["links"])
        slot = {b: k for k, b in enumerate(order)}
        ranked = [sorted(m["influences"][v], key=lambda x: -x[1]) for v in weighted]
        pad = lambda inf, n: inf + [(order[0], 0.0)] * (n - len(inf))
        full = np.array([[slot[b] for b, _ in pad(inf, width)] for inf in ranked])
        full_w = np.array([[w for _, w in pad(inf, width)] for inf in ranked])
        top = np.array([[character.bone_index[b] for b, _ in pad(inf[:max_influences], max_influences)] for inf in ranked])
        top_w = np.array([[w for _, w in pad(inf[:max_influences], max_influences)] for inf in ranked])
        points = np.c_[m["positions"][weighted], np.ones(len(weighted))]
        bind_points = np.c_[character.point(Character.bind_points(m, bind))[weighted], np.ones(len(weighted))]
        meshes.append((m, order, full, full_w / full_w.sum(1, keepdims=True), top, top_w / top_w.sum(1, keepdims=True), points, bind_points))
    stretched, worst = {}, (0.0, 0)
    for frame, t in enumerate(clip["times"]):
        literal = scene.globals(scene.locals_at(clip, t))
        for bone in character.deforming:
            sv = np.linalg.svd(literal[bone][:3, :3], compute_uv=False)
            name = display_name(scene.models[bone])
            stretched[name] = max(stretched.get(name, 0.0), float(sv.max() / sv.min() - 1))
        engine_mats = np.array(character.skinning(clip_worlds[frame]))
        for m, order, full, full_w, top, top_w, points, bind_points in meshes:
            literal_mats = np.array([literal[b] @ m["links"][b] for b in order])
            exact = np.einsum("nk,nki->ni", full_w, np.einsum("nkij,nj->nki", literal_mats[full], points))[:, :3]
            engine = np.einsum("nk,nki->ni", top_w, np.einsum("nkij,nj->nki", engine_mats[top], bind_points))[:, :3]
            error = np.linalg.norm(character.point(exact) - engine, axis=1).max()
            if error > worst[0]: worst = (float(error), frame)
    bad = sorted(((v, n) for n, v in stretched.items() if v > 0.05), reverse=True)
    if bad:
        print(f"  {len(bad)} bones stretch or shear in the FBX and are fitted to uniform scale: "
              + ", ".join(f"{n} {v:.0%}" for v, n in bad[:4]) + (", ..." if len(bad) > 4 else ""))
    print(f"  the fitted rig differs from the FBX composed literally by at most {worst[0] * 100:.1f} cm (frame {worst[1]})")

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source"); parser.add_argument("output")
    parser.add_argument("--name", default=None)
    parser.add_argument("--height", type=float, default=1.8, help="character height in metres (default 1.8)")
    parser.add_argument("--colors", help="JSON {material or mesh name: [r, g, b]} in linear 0..1, for colours FBX lost")
    parser.add_argument("--pose-frame", type=int, default=0, help="clip frame baked into NAME_pose.vmesh")
    parser.add_argument("--rekey-walk", action="store_true",
                        help="replace the walk clip with a re-keyed one (see walk_cycle.py); keep the original as walk_authored")
    parser.add_argument("--gait", default="", help="re-keyed walk settings, e.g. crouch=0.035,bob=0.02 (walk_cycle.Gait)")
    args = parser.parse_args()
    source = pathlib.Path(args.source); out = pathlib.Path(args.output); out.mkdir(parents=True, exist_ok=True)
    name = args.name or source.stem.lower()
    scene = FbxScene(read_fbx(source))
    filled = fill_unweighted(scene)
    character = Character(scene, args.height)
    character.add_half_joints()
    overrides = json.loads(pathlib.Path(args.colors).read_text()) if args.colors else {}
    missing = set()
    def colors(mesh, material):
        for key in ((display_name(material) if material is not None else None), mesh["name"]):
            if key in overrides: return tuple(float(c) for c in overrides[key]) + (1.0,)
        color = scene.material_color(material) if material is not None else None
        if color is None:
            missing.add(display_name(material) if material is not None else mesh["name"])
            color = np.array([0.5, 0.5, 0.5])
        return tuple(float(c) for c in color) + (1.0,)
    rows, faces = build_mesh(character, colors)
    clips, notes = [], []
    for clip in scene.clips:
        fps = 1.0 / np.median(np.diff(clip["times"])) if len(clip["times"]) > 1 else 24.0
        frames, worlds = sample_clip(scene, character, clip)
        clip_name = clip["name"].lower()
        authored = dict(name=clip_name, fps=round(fps, 3), frames=frames, worlds=worlds, loop=True,
                        speed=walking_speed(character, rows, worlds, fps))
        if args.rekey_walk and clip_name == "walk":
            import walk_cycle
            gait = walk_cycle.Gait(**{k: float(v) for k, v in (item.split("=") for item in args.gait.split(",") if item)})
            hands = hand_grips(scene, character, worlds[0], gait.dip, gait.lower)
            new_worlds, short = walk_cycle.rekey_walk(character, worlds, fps, sole_points(character, rows), gait, hands)
            reach = held_out_of_body(scene, character, hands, new_worlds[0])
            if reach:
                new_worlds, short = walk_cycle.rekey_walk(character, worlds, fps, sole_points(character, rows), gait, hands)
                notes.append(f"the held mesh was {reach * 100:.1f} cm into the body; the holding hand reaches that much further along it")
            new_worlds = [character.with_halves(w) for w in new_worlds]
            clips.append(dict(name="walk", fps=round(fps, 3), frames=frames_from_worlds(character, new_worlds),
                              worlds=new_worlds, loop=True, speed=gait.speed))
            authored["name"] = "walk_authored"
            notes.append(f"walk re-keyed at {gait.speed:g} m/s (IK fell short by at most {short * 100:.1f} cm); "
                         "the original is walk_authored")
        clips.append(authored)
    write_mesh(out / f"{name}.vmesh", name, source.name, rows, faces, rig=f"{name}.vrig")
    write_rig(out / f"{name}.vrig", name, source.name, character, clips)
    if clips:
        frame = min(args.pose_frame, len(clips[0]["worlds"]) - 1)
        pose_rows, pose_faces = build_mesh(character, colors, clips[0]["worlds"][frame])
        write_mesh(out / f"{name}_pose.vmesh", f"{name} ({clips[0]['name']} frame {frame})", source.name, pose_rows, pose_faces)
    print(f"{name}: {len(rows)} vertices, {len(faces)} triangles, {len(character.names)} bones, "
          f"{len(clips)} clip(s): " + ", ".join(f"{c['name']} {len(c['frames'])} frames at {c['fps']:g} fps, "
                                              f"{c['speed']:.2f} m/s" for c in clips))
    if missing:
        print("no colour in the FBX for: " + ", ".join(sorted(missing)) + " (grey; pass --colors or textures)")
    if filled: print(f"  {filled} vertices had no skin weights; they take their nearest weighted neighbour's")
    for note in notes: print("  " + note)
    authored = next((c for c in clips if c["name"] != "walk" or not args.rekey_walk), None)
    report_fidelity(scene, character, authored["worlds"] if authored else [])

if __name__ == "__main__":
    main()
