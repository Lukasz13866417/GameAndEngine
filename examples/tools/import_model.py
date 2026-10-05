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

def between(upper, lower, f):
    """A frame part way through a joint: the rotation a fraction f of the way
    from the upper bone's to the lower bone's (slerp), at the lower bone's
    head; the scale as far between the bones'."""
    su, sl = np.cbrt(np.linalg.det(upper[:3, :3])), np.cbrt(np.linalg.det(lower[:3, :3]))
    qu, ql = quaternion(upper[:3, :3] / su), quaternion(lower[:3, :3] / sl)
    if qu @ ql < 0: ql = -ql
    angle = np.arccos(np.clip(qu @ ql, -1, 1))
    q = qu if angle < 1e-6 else (np.sin((1 - f) * angle) * qu + np.sin(f * angle) * ql) / np.sin(angle)
    out = np.eye(4); out[:3, :3] = rotation_matrix(q / np.linalg.norm(q)) * (su + (sl - su) * f)
    out[:3, 3] = lower[:3, 3]
    return out


def decompose(m):
    """(translation, quaternion, uniform scale) of a similarity matrix."""
    scale = np.cbrt(np.linalg.det(m[:3, :3]))
    return m[:3, 3].copy(), quaternion(m[:3, :3] / scale), float(scale)

class Character:
    """The FBX scene in engine space: deforming bones only, similarity poses.
    With a `skeleton` (body.Skeleton, designed from this character), its bones
    stand in for the FBX's: the skeleton's names, hierarchy and rest worlds,
    and clips retargeted onto it."""

    def __init__(self, scene, height, skeleton=None):
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
        self.skeleton = None
        self.halves = []
        if skeleton is not None:
            self.original = skeleton.original
            self.skeleton = skeleton
            self.deforming = list(skeleton.deforming)
            self.names = list(skeleton.names)
            self.parents = list(skeleton.parents)
            self.bone_index = {b: k + 1 for k, b in enumerate(self.deforming)}
            self.rest = self.with_halves([m.copy() for m in skeleton.rest])
            return
        self.names = ["root"] + [display_name(scene.models[b]) for b in self.deforming]
        index = {b: k + 1 for k, b in enumerate(self.deforming)}
        self.parents = [-1] + [index.get(parent[b], 0) for b in self.deforming]
        self.bone_index = index
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
        if self.skeleton is not None:
            import body
            return self.with_halves(body.retarget(self.skeleton, self.original, globals_, self.world))
        worlds = [np.eye(4)]
        for b in self.deforming: worlds.append(self.world(globals_[b]))
        return self.with_halves(worlds)

    # Joints that get a half-angle helper: (name, upper bone, lower bone).
    # Joints with volume helpers, and how far through each joint its helpers
    # turn. An elbow folds furthest (the soldier's holding arm by 128
    # degrees), so it gets three; knees and shoulders one, at the half.
    HALF_JOINTS = (("elbow", "DEF-upper_arm.{}.001", "DEF-forearm.{}", (.25, .5, .75)),
                   ("knee", "DEF-thigh.{}.001", "DEF-shin.{}", (.5,)),
                   ("shoulder", "DEF-shoulder.{}", "DEF-upper_arm.{}", (.5,)))
    HELPER_NAMES = {.25: "QUARTER", .5: "HALF", .75: "THREEQUARTER"}

    def add_half_joints(self):
        """Volume helpers: at each elbow, knee and shoulder, bones that turn a
        set fraction of the way through the joint (HALF- at one half;
        QUARTER- and THREEQUARTER- too at the elbows). Linear blend skinning
        averages the two bones' matrices across a joint, which shrinks a bent
        elbow toward its centre; skin shared by the two bones moves onto the
        helpers nearest its share (share_halves), which turn rigidly, so no
        blend spans more than the gap between two of them."""
        for joint, upper, lower, steps in self.HALF_JOINTS:
            for side in "LR":
                u, l = upper.format(side), lower.format(side)
                if u not in self.names or l not in self.names: continue
                for f in steps:
                    self.names.append(f"{self.HELPER_NAMES[f]}-{joint}.{side}"); self.parents.append(self.names.index(u))
                    self.halves.append((len(self.names) - 1, self.names.index(u), self.names.index(l), f))
        self.rest = self.with_halves(self.rest)

    def with_halves(self, worlds):
        """Bone worlds with the joint helpers set from their joints' bones:
        each moves (from its rest) its fraction of the way between how the two
        bones moved, about the joint. A helper's rest is that far between the
        bones' rests."""
        worlds = list(worlds[:len(self.deforming) + 1])
        for h, u, l, f in self.halves:
            if h >= len(self.rest):                         # setting up the helper's own rest
                worlds.append(between(worlds[u], worlds[l], f)); continue
            move = between(worlds[u] @ np.linalg.inv(self.rest[u]), worlds[l] @ np.linalg.inv(self.rest[l]), f)
            joint_rest, joint_now = self.rest[l][:3, 3], worlds[l][:3, 3]
            move[:3, 3] = joint_now - move[:3, :3] @ joint_rest
            worlds.append(move @ self.rest[h])
        return worlds

    def share_halves(self, skin):
        """Skin [(bone, weight)] with each joint's shared weight on its helpers.
        A vertex weighted a to the upper bone and b to the lower should turn
        b / (a + b) of the way through the joint; its a + b goes to the two
        steps either side of that share (the bones at 0 and 1, the helpers
        between), so a 50/50 vertex turns rigidly with the half helper."""
        skin = dict(skin)
        joints = {}
        for h, u, l, f in self.halves: joints.setdefault((u, l), []).append((f, h))
        for (u, l), helpers in joints.items():
            a, b = skin.get(u, 0.0), skin.get(l, 0.0)
            if min(a, b) < 0.01: continue
            steps = [(0.0, u)] + sorted(helpers) + [(1.0, l)]
            share, total = b / (a + b), a + b
            k = next(k for k in range(len(steps) - 1) if share <= steps[k + 1][0])
            (f0, b0), (f1, b1) = steps[k], steps[k + 1]
            t = (share - f0) / (f1 - f0)
            del skin[u], skin[l]
            skin[b0] = skin.get(b0, 0.0) + (1 - t) * total; skin[b1] = skin.get(b1, 0.0) + t * total
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
            color = colors(mesh, mesh["materials"][material_index] if mesh["materials"] else None, face)
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

def write_rig(path, name, source, character, clips, lights=()):
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
    if lights:
        lines.append("lights = [")
        for light in lights:
            lines.append(f"    {{ name = {quote(light['name'])}; "
                         f"position = {vector(light['position'])}; direction = {vector(light['direction'])}; "
                         f"color = {vector(light.get('color', [1, 1, 1]))}; inner = {f32(light.get('inner', 12))}; "
                         f"outer = {f32(light.get('outer', 20))}; range = {f32(light.get('range', 8))}; }},")
        lines.append("];")
    pathlib.Path(path).write_text("\n".join(lines) + "\n")

def lens_faces(scene, character, lights):
    """The faces that make each light's lens (`lens` metres round its
    position, facing along it), so they can glow in its colour: {(mesh id,
    face): colour}."""
    bind = scene.bind_globals(); glowing = {}
    for light in lights:
        radius = light.get("lens", 0)
        if not radius: continue
        at, toward = np.array(light["position"], float), np.array(light["direction"], float)
        toward /= np.linalg.norm(toward)
        glow = tuple(float(c) * 2 for c in light.get("color", [1, 1, 1])) + (1.0,)
        for mesh in scene.meshes:
            p = character.point(Character.bind_points(mesh, bind))[mesh["corners"].reshape(-1, 3)]
            normals = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
            normals /= np.linalg.norm(normals, axis=1, keepdims=True) + 1e-12
            for f in np.nonzero((np.linalg.norm(p.mean(1) - at, axis=1) < radius) & (normals @ toward > .5))[0]:
                glowing[(id(mesh), int(f))] = glow
    return glowing

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

def drop_doubled_faces(scene):
    """A Blender mesh can hold the same triangle twice, facing opposite ways:
    the soldier's shoulder flashlight is a shell built twice over. Drawn
    together, the two copies fight for the same pixels and the surface comes
    out in stripes of light and dark. Of each such pair, the triangle facing
    away from the middle of its doubled region is kept. Returns the
    triangles dropped."""
    dropped = 0
    for mesh in scene.meshes:
        p = mesh["positions"]
        key = {}; same = np.array([key.setdefault(tuple(q), v) for v, q in enumerate(np.round(p, 6))])
        triangles = same[mesh["corners"].reshape(-1, 3)]
        groups = {}
        for f, t in enumerate(triangles): groups.setdefault(frozenset(t), []).append(f)
        pairs = [fs for fs in groups.values() if len(fs) == 2 and len(frozenset(triangles[fs[0]])) == 3]
        if not pairs: continue
        # Doubled regions: pairs that share a vertex, and the middle of each.
        parent = {}
        def root(a):
            while parent.setdefault(a, a) != a: a = parent[a]
            return a
        for fs in pairs:
            a, b, c = triangles[fs[0]]; parent[root(b)] = root(a); parent[root(c)] = root(a)
        region = {}
        for fs in pairs: region.setdefault(root(triangles[fs[0]][0]), []).append(fs)
        drop = set()
        for members in region.values():
            middle = np.mean([p[mesh["corners"][3 * fs[0]:3 * fs[0] + 3]].mean(0) for fs in members], axis=0)
            for fs in members:
                def outward(f):
                    a, b, c = p[mesh["corners"][3 * f:3 * f + 3]]
                    return np.cross(b - a, c - a) @ ((a + b + c) / 3 - middle)
                drop.add(min(fs, key=outward))
        keep = np.array([f for f in range(len(triangles)) if f not in drop])
        corners = (3 * keep[:, None] + np.arange(3)).reshape(-1)
        mesh["corners"] = mesh["corners"][corners]
        mesh["normals"] = mesh["normals"][corners]
        mesh["face_material"] = np.asarray(mesh["face_material"])[keep]
        dropped += len(drop)
    return dropped

FACE_PARTS = ("jaw", "ear", "lip", "cheek", "nose", "eye", "brow", "lid", "chin", "tongue", "teeth", "forehead", "temple")

def keep_face_weights_on_head(scene):
    """Blender's automatic weights let things near the face follow the face's
    bones: the soldier's shoulder flashlight followed his jaw by about half.
    When the head turns against the shoulders, as in the walk, such a piece
    bends. Only the mesh carrying the most head-bone weight (the head's skin)
    keeps face-bone weights; elsewhere they go to the vertex's strongest other
    bone. Returns the vertices changed."""
    names = {b: display_name(scene.models[b]) for b in scene.models}
    face = lambda b: names.get(b, "").removeprefix("DEF-").split(".")[0] in FACE_PARTS
    head = lambda b: names.get(b, "") == "DEF-spine.006" or face(b)
    if not scene.meshes: return 0
    skin = max(scene.meshes, key=lambda m: sum(w for inf in m["influences"] for b, w in inf if head(b)))
    changed = 0
    for mesh in scene.meshes:
        if mesh is skin: continue
        for v, inf in enumerate(mesh["influences"]):
            moved = sum(w for b, w in inf if face(b))
            rest = [(b, w) for b, w in inf if not face(b)]
            if not moved: continue
            if not rest: rest = [(next(b for b in names if names[b] == "DEF-spine.006"), 0.0)]
            strongest = max(range(len(rest)), key=lambda k: rest[k][1])
            rest[strongest] = (rest[strongest][0], rest[strongest][1] + moved)
            mesh["influences"][v] = rest; changed += 1
    return changed

def hand_grips(scene, character, first_frame, dip=0.0, lower=0.0, reach=0.0):
    """Grips for a mesh held wholly by one hand (the soldier's rifle): that
    hand's fingers close on it, and the other hand takes its far hole the way
    the holding hand takes the near one (or, without two holes, supports it
    from below). The held mesh's far end dips `dip` degrees about its rear,
    and the whole of it sits `lower` metres lower and `reach` metres further
    forward along its length."""
    import grip
    bind = scene.bind_globals()
    hands_mesh = next((m for m in scene.meshes if m["name"].lower().startswith("hand")), None)
    if hands_mesh is None: return {}
    points = character.point(Character.bind_points(hands_mesh, bind))
    bones = np.array([character.bone_index[max(inf, key=lambda x: x[1])[0]] for inf in hands_mesh["influences"]])
    found = held_mesh(scene, character)
    if found is not None:
        mesh, h = found
        side = character.names[h][-1]; other = "R" if side == "L" else "L"
        held = character.point(Character.bind_points(mesh, bind))
        triangles = held[mesh["corners"].reshape(-1, 3)]
        holding, support = grip.Hand(character, side, points, bones), grip.Hand(character, other, points, bones)
        holding_world = first_frame[h] @ np.linalg.inv(character.rest[h])
        placement = grip.hole_placement(holding, support, held, triangles, holding_world)
        if placement is None: placement = grip.support_placement(support, held, triangles, holding_world)
        on_support = placement @ np.linalg.inv(character.rest[support.hand])
        frame = grip.held_frame(held, character.rest[h][:3, 3], holding_world)
        rear = frame[:3, 3] + frame[:3, 0] * ((held - frame[:3, 3]) @ frame[:3, 0]).min()
        carry = np.eye(4); carry[:3, :3] = grip._rotation(frame[:3, 2], -dip)
        carry[:3, 3] = rear - carry[:3, :3] @ rear - frame[:3, 1] * lower + frame[:3, 0] * reach
        # Both thumbs point along it, toward its far end (the muzzle), drooping
        # toward the floor of the clip's first frame where they can.
        forward = grip.long_axis(held, character.rest[h][:3, 3])
        down = -np.linalg.inv(first_frame[h] @ np.linalg.inv(character.rest[h]))[:3, 1]
        holding.aim_thumb(triangles, np.eye(4), forward, down)
        support.aim_thumb(triangles, on_support, forward, down)
        return {side: dict(hand=holding, curls=holding.close(triangles), support=None, carry=carry),
                other: dict(hand=support, curls=support.close(triangles, on_support), support=placement)}
    return {}

def even_elbows(scene, character, width=0.12, outer_width=0.05):
    """The soldier's sleeves hand over from upper arm to forearm unevenly, from
    9 cm above the elbow to 12 cm below it, so a bent arm folds off the joint.
    On each arm vertex the weight the two arm bones share is re-split by a
    smoothstep of the distance along the arm from the elbow, centred on the
    joint, over +-width metres on the inside of the fold (the crease) and
    +-outer_width on the outside. The back of a forearm stays with the
    forearm right up to the point of the elbow, so a folded arm keeps its
    thickness to the joint. An arm in the T-pose folds toward the front, the
    way the character faces. A rigid piece (an elbow pad, whose forearm share
    barely varies) stays rigid and takes the share the sleeve has under its
    middle, so it stays on it; other bones' weights are untouched. Returns
    the vertices changed."""
    bind = scene.bind_globals(); rest = character.rest; changed = 0
    for side in ("L", "R"):
        try: s0, e0, w0 = (rest[character.names.index(f"DEF-{n}.{side}")][:3, 3] for n in ("upper_arm", "forearm", "hand"))
        except ValueError: continue
        up_dir = (e0 - s0) / np.linalg.norm(e0 - s0); fore_dir = (w0 - e0) / np.linalg.norm(w0 - e0)
        # The inside of the fold: the front of the arm (+Z, the way the character faces).
        inside = np.array([0.0, 0.0, 1.0]) - up_dir[2] * up_dir; inside /= np.linalg.norm(inside)
        names = {b: display_name(scene.models[b]) for b in scene.models}
        upper = lambda b: names.get(b, "").startswith(f"DEF-upper_arm.{side}")
        fore = lambda b: names.get(b, "").startswith(f"DEF-forearm.{side}")
        for mesh in editable(scene):
            points = character.point(Character.bind_points(mesh, bind))
            shares, picks = [], []
            for v, inf in enumerate(mesh["influences"]):
                u = sum(w for b, w in inf if upper(b)); f = sum(w for b, w in inf if fore(b)); total = sum(w for _, w in inf)
                if total > 0 and (u + f) / total > 0.5 and u + f > 0: shares.append(f / (u + f)); picks.append(v)
            if not picks: continue                                  # no arm here
            rigid = np.ptp(shares) < 0.2                            # an elbow pad, say
            joint_upper = next((b for b in mesh["links"] if names[b] == f"DEF-upper_arm.{side}.001"), None)
            joint_fore = next((b for b in mesh["links"] if names[b] == f"DEF-forearm.{side}"), None)
            def share_at(p):
                d = p - e0
                axis = fore_dir if d @ fore_dir > 0 else up_dir
                along = d @ axis
                radial = d - along * axis
                facing = radial @ inside / (np.linalg.norm(radial) + 1e-9)   # 1 inside the fold, -1 outside
                reach = outer_width + (width - outer_width) * (1 + facing) / 2
                x = np.clip((along + reach) / (2 * reach), 0, 1); return x * x * (3 - 2 * x)
            middle = share_at(points[picks].mean(0)) if rigid else None
            for v in picks:
                inf = mesh["influences"][v]
                target = middle if rigid else share_at(points[v])
                u = sum(w for b, w in inf if upper(b)); f = sum(w for b, w in inf if fore(b))
                if abs(f / (u + f) - target) < 0.02: continue
                # Keep each side's split between its two segments; a side that had no weight gets the joint's segment.
                new = [(b, w) for b, w in inf if not upper(b) and not fore(b)]
                for keep, share, joint in ((upper, (1 - target) * (u + f), joint_upper), (fore, target * (u + f), joint_fore)):
                    own = [(b, w) for b, w in inf if keep(b)]; total = sum(w for _, w in own)
                    if share <= 1e-6: continue
                    if total > 0: new += [(b, share * w / total) for b, w in own]
                    elif joint is not None: new.append((joint, share))
                mesh["influences"][v] = new; changed += 1
    return changed

def editable(scene):
    """The meshes the weight and shape fixes apply to: not one remodelled by body.py."""
    return [m for m in scene.meshes if not m.get("generated")]

def smooth_shoulders(scene, character, radius=0.15, rounds=4):
    """Around each shoulder joint, skin weights are blended toward their mesh
    neighbours' (half-strength at the joint, fading to nothing at `radius`
    metres), a few rounds. Where the chest meets the upper arm the soldier's
    weights jump between neighbouring vertices, and with the arm swung far
    from the T-pose those jumps fold the skin into bumps. Meshes bound to a
    single bone are left alone. Returns the vertices changed."""
    bind = scene.bind_globals(); changed = 0
    for side in ("L", "R"):
        if f"DEF-upper_arm.{side}" not in character.names: continue
        joint = character.rest[character.names.index(f"DEF-upper_arm.{side}")][:3, 3]
        for mesh in editable(scene):
            if len({b for inf in mesh["influences"] for b, _ in inf}) < 2: continue
            points = character.point(Character.bind_points(mesh, bind))
            reach = 1 - np.linalg.norm(points - joint, axis=1) / radius
            near = np.nonzero(reach > 0)[0]
            if not len(near): continue
            neighbours = [set() for _ in range(len(points))]
            for a, b, c in mesh["corners"].reshape(-1, 3):
                for x, y in ((a, b), (b, c), (c, a)): neighbours[x].add(y); neighbours[y].add(x)
            weights = [{b: w / sum(x for _, x in inf) for b, w in inf} if inf else {} for inf in mesh["influences"]]
            for _ in range(rounds):
                blended = list(weights)
                for v in near:
                    if not neighbours[v]: continue
                    mean = {}
                    for n in neighbours[v]:
                        for b, w in weights[n].items(): mean[b] = mean.get(b, 0.0) + w / len(neighbours[v])
                    k = 0.5 * reach[v]
                    mix = {b: (1 - k) * weights[v].get(b, 0.0) + k * mean.get(b, 0.0) for b in set(weights[v]) | set(mean)}
                    total = sum(mix.values()); blended[v] = {b: w / total for b, w in mix.items() if w / total > 1e-3}
                weights = blended
            for v in near: mesh["influences"][v] = sorted(weights[v].items(), key=lambda x: -x[1])[:8]
            changed += len(near)
    return changed

def firm_underarms(scene, character, start=0.02, end=0.10, reach=0.11, length=0.16):
    """Under the upper arm, near the armpit, the soldier's skin was shared with
    the torso several centimetres out along the arm, so with the arm raised
    the underside lagged toward the body and sagged into a hollow where the
    tricep should bulge. On the tricep's side of the T-pose arm (below and
    behind, opposite the elbow's fold), fading out toward the sides, the arm's
    share rises to a smoothstep from `start` to `end` metres along the arm from
    the shoulder joint, out to `length`; within `reach` of the bone, and only
    up, never down. Returns the vertices changed."""
    bind = scene.bind_globals(); names = {b: display_name(scene.models[b]) for b in scene.models}
    changed = 0
    for side in ("L", "R"):
        try: joint, elbow = (character.rest[character.names.index(f"DEF-{n}.{side}")][:3, 3] for n in ("upper_arm", "forearm"))
        except ValueError: continue
        axis = (elbow - joint) / np.linalg.norm(elbow - joint)
        down = np.array([0.0, -1.0, -1.0])                # the tricep: below and behind
        down = down - (down @ axis) * axis; down /= np.linalg.norm(down)
        arm = lambda b: names.get(b, "").startswith((f"DEF-upper_arm.{side}", f"DEF-forearm.{side}"))
        default = next((b for b in scene.models if names[b] == f"DEF-upper_arm.{side}"), None)
        for mesh in editable(scene):
            points = character.point(Character.bind_points(mesh, bind))
            if not any(arm(b) for inf in mesh["influences"] for b, _ in inf): continue
            for v, inf in enumerate(mesh["influences"]):
                d = points[v] - joint; along = d @ axis; radial = d - along * axis; distance = np.linalg.norm(radial)
                if not (start - 0.02 < along < length) or distance > reach or not inf: continue
                under = np.clip((radial @ down / (distance + 1e-9) - 0.2) / 0.6, 0, 1)
                if under <= 0: continue
                x = np.clip((along - start) / (end - start), 0, 1)
                total = sum(w for _, w in inf); share = sum(w for b, w in inf if arm(b)) / total
                target = share + under * (x * x * (3 - 2 * x) - share)
                if target <= share + 0.01: continue
                armed = {b: w for b, w in inf if arm(b)}; body = {b: w for b, w in inf if not arm(b)}
                a, t = sum(armed.values()), sum(body.values())
                new = [(b, target * total * w / a) for b, w in armed.items()] if a > 0 else [(default, target * total)]
                new += [(b, (1 - target) * total * w / t) for b, w in body.items()] if t > 0 else []
                mesh["influences"][v] = [(b, w) for b, w in new if w > 1e-6]; changed += 1
    return changed

def move_vertices(scene, character, points, shift):
    """Moves each mesh's vertices by `shift` ({id(mesh): offsets in character
    space}, from `points`, its bind-pose points there) back in its own FBX
    space; each corner normal turns as its vertex's faces did. Returns the
    largest move in metres."""
    bind = scene.bind_globals()
    largest = 0.0
    for mesh in scene.meshes:
        d = shift[id(mesh)]
        if not d.any(): continue
        largest = max(largest, float(np.linalg.norm(d, axis=1).max()))
        # Back to the mesh's own FBX space; each corner normal turns as its vertex's faces did.
        world = ((points[id(mesh)] + d - character.offset) / character.scale) @ character.linear
        local = (np.c_[world, np.ones(len(world))] @ np.linalg.inv(bind[mesh["model"]]).T)[:, :3]
        corners = mesh["corners"].reshape(-1, 3)
        def vertex_normals(p):
            faces = np.cross(p[corners[:, 1]] - p[corners[:, 0]], p[corners[:, 2]] - p[corners[:, 0]])
            out = np.zeros_like(p)
            for c in range(3): np.add.at(out, corners[:, c], faces)
            return out / (np.linalg.norm(out, axis=1, keepdims=True) + 1e-12)
        before, after = vertex_normals(mesh["positions"]), vertex_normals(local)
        normals = mesh["normals"].reshape(-1, 3).copy()
        for corner, v in enumerate(mesh["corners"]):
            a, b = before[v], after[v]; c = a @ b
            if c > 1 - 1e-9 or c < -0.5: continue
            cross = np.cross(a, b); skew = np.array([[0, -cross[2], cross[1]], [cross[2], 0, -cross[0]], [-cross[1], cross[0], 0]])
            normals[corner] = (np.eye(3) + skew + skew @ skew / (1 + c)) @ normals[corner]
        mesh["positions"] = local; mesh["normals"] = normals.reshape(mesh["normals"].shape)
    return largest

def flatten_deltoids(scene, character, start=0.02, end=0.20, allow=0.003):
    """--flatten-deltoids: the soldier's sleeves bulge on top 8-11 cm out from
    the shoulder joint, about 2 cm above a straight taper. With the arm held
    forward and down, the part nearer the joint follows the arm only partly,
    and the bulge stands out of the arm's top line as a step. Per direction
    around each upper arm, the sleeve's distance from the bone may not exceed
    the straight line from its distance `start` metres along the arm to its
    distance at `end` (+ allow); vertices beyond it move toward the bone.
    Whatever lies on the sleeve there (the soldier's shoulder yoke) moves with
    the sleeve under it, and normals turn with their faces. Edits the scene's
    meshes in place. Returns the sleeve vertices moved and the largest move in
    metres."""
    bind = scene.bind_globals(); names = {b: display_name(scene.models[b]) for b in scene.models}
    sectors = np.radians(np.arange(-180, 180, 30) + 15)
    points = {id(mesh): character.point(Character.bind_points(mesh, bind)) for mesh in scene.meshes}
    shift = {id(mesh): np.zeros_like(points[id(mesh)]) for mesh in scene.meshes}
    moved_count = 0
    for side in ("L", "R"):
        try: joint, elbow = (character.rest[character.names.index(f"DEF-{n}.{side}")][:3, 3] for n in ("upper_arm", "forearm"))
        except ValueError: continue
        axis = (elbow - joint) / np.linalg.norm(elbow - joint)
        up = np.array([0.0, 1.0, 0.0]) - axis[1] * axis; up /= np.linalg.norm(up); front = np.cross(axis, up)
        on_arm = {id(mesh): np.array([any(names.get(b, "").startswith(f"DEF-upper_arm.{side}") for b, _ in inf) for inf in mesh["influences"]])
                  for mesh in scene.meshes}
        def place(p):
            along = (p - joint) @ axis; radial = p - joint - np.outer(along, axis)
            return along, radial, np.linalg.norm(radial, axis=1), np.arctan2(radial @ front, radial @ up)
        def profile(at):                                  # largest distance from the bone per direction, in a 2 cm slab
            best = np.full(len(sectors), np.nan)
            for mesh in editable(scene):
                along, _, distance, angle = place(points[id(mesh)][on_arm[id(mesh)]])
                for k, c in enumerate(sectors):
                    pick = (np.abs(along - at) < 0.01) & (distance < 0.16) & (np.abs((angle - c + np.pi) % (2 * np.pi) - np.pi) < np.radians(25))
                    if pick.any(): best[k] = np.fmax(best[k], distance[pick].max())
            known = np.nonzero(~np.isnan(best))[0]
            return np.interp(np.arange(len(best)), known, best[known], period=len(best)) if len(known) else None
        near, far = profile(start), profile(end)
        if near is None or far is None: continue
        sleeve, pulled = [], []
        for mesh in editable(scene):
            vertices = np.nonzero(on_arm[id(mesh)])[0]
            along, radial, distance, angle = place(points[id(mesh)][vertices])
            k = (angle - sectors[0]) / np.radians(30) % len(sectors); i = k.astype(int) % len(sectors); j = (i + 1) % len(sectors); f = k - np.floor(k)
            x = (along - start) / (end - start)
            cap = (near[i] * (1 - f) + near[j] * f) * (1 - x) + (far[i] * (1 - f) + far[j] * f) * x + allow
            pull = (x > 0) & (x < 1) & (distance < 0.16) & (distance > cap)
            shift[id(mesh)][vertices[pull]] = (radial[pull] * (cap[pull] / distance[pull] - 1)[:, None])
            moved_count += int(pull.sum())
            sleeve.append(points[id(mesh)][vertices]); pulled.append(shift[id(mesh)][vertices])
        sleeve, pulled = np.concatenate(sleeve), np.concatenate(pulled)
        # Off-arm vertices within 4 cm of the sleeve follow its nearest vertices (fading out from 2 cm).
        for mesh in editable(scene):
            others = np.nonzero(~on_arm[id(mesh)])[0]
            along, _, distance, _ = place(points[id(mesh)][others])
            zone = others[(along > start - 0.02) & (along < end + 0.02) & (distance < 0.2)]
            for v in zone:
                gap = np.linalg.norm(sleeve - points[id(mesh)][v], axis=1); nearest = np.argsort(gap)[:4]
                if gap[nearest[0]] > 0.04: continue
                weights = 1 / (gap[nearest] + 1e-3)
                shift[id(mesh)][v] = (weights @ pulled[nearest]) / weights.sum() * np.clip((0.04 - gap[nearest[0]]) / 0.02, 0, 1)
    largest = move_vertices(scene, character, points, shift)
    return moved_count, largest

def shape_arms(scene, character, tricep=0.015, slim=0.2):
    """--shape-arms: two changes to the soldier's sleeves, which are plain
    tubes along the bones.
    - A tricep: the back and underside of each upper arm (opposite the
      elbow's fold) swells by up to `tricep` metres between 4 and 22 cm out
      from the shoulder, highest in the middle, so with the arm raised its
      underside is gently convex rather than a straight line sagging into the
      armpit.
    - A slimmer forearm by the elbow: from 2 cm past the elbow, each forearm's
      cross-section narrows by up to `slim` (a share of its width) about its
      own middle, most at 6 to 10 cm and none from 18 cm, so the forearm is
      no wider than the elbow it folds against.
    Returns the vertices moved and the largest move in metres."""
    bind = scene.bind_globals(); names = {b: display_name(scene.models[b]) for b in scene.models}
    points = {id(mesh): character.point(Character.bind_points(mesh, bind)) for mesh in scene.meshes}
    shift = {id(mesh): np.zeros_like(points[id(mesh)]) for mesh in scene.meshes}
    smooth = lambda x: (lambda c: c * c * (3 - 2 * c))(np.clip(x, 0, 1))
    moved = 0
    for side in ("L", "R"):
        try: s0, e0, w0 = (character.rest[character.names.index(f"DEF-{n}.{side}")][:3, 3] for n in ("upper_arm", "forearm", "hand"))
        except ValueError: continue
        up_dir = (e0 - s0) / np.linalg.norm(e0 - s0); fore_dir = (w0 - e0) / np.linalg.norm(w0 - e0)
        back = np.array([0.0, -1.0, -1.0]) - (np.array([0.0, -1.0, -1.0]) @ up_dir) * up_dir; back /= np.linalg.norm(back)
        on = lambda inf, prefix: sum(w for b, w in inf if names.get(b, "").startswith(prefix)) / max(sum(w for _, w in inf), 1e-9)
        for mesh in editable(scene):
            p = points[id(mesh)]; inf = mesh["influences"]
            upper = np.array([on(i, f"DEF-upper_arm.{side}") for i in inf]); fore = np.array([on(i, f"DEF-forearm.{side}") for i in inf])
            if not (upper.any() or fore.any()): continue
            # The tricep: a smooth swell on the back-under side of the upper arm.
            d = p - s0; along = d @ up_dir; radial = d - np.outer(along, up_dir); distance = np.linalg.norm(radial, axis=1)
            facing = (radial @ back) / (distance + 1e-9)
            profile = np.sin(np.pi * np.clip((along - 0.04) / 0.18, 0, 1))
            swell = tricep * profile * smooth((facing - 0.1) / 0.6) * (upper > 0.5) * (distance < 0.16)
            pick = swell > 1e-4
            shift[id(mesh)][pick] += radial[pick] / distance[pick, None] * swell[pick, None]
            # The forearm by the elbow: narrower about the middle of its own cross-section.
            d = p - e0; along = d @ fore_dir; radial = d - np.outer(along, fore_dir)
            sleeve = (fore > 0.5) & (along > 0) & (along < 0.2) & (np.linalg.norm(radial, axis=1) < 0.16)
            if sleeve.any():
                middles = np.array([radial[sleeve & (np.abs(along - a) < 0.015)].mean(0) for a in along[sleeve]])
                amount = slim * smooth((along[sleeve] - 0.02) / 0.04) * (1 - smooth((along[sleeve] - 0.10) / 0.08))
                shift[id(mesh)][sleeve] += -(radial[sleeve] - middles) * amount[:, None]
            moved += int(pick.sum() + sleeve.sum())
    largest = move_vertices(scene, character, points, shift)
    return moved, largest

def held_mesh(scene, character):
    """The mesh bound wholly to one hand (the soldier's rifle) and that hand's bone index, or None."""
    for mesh in scene.meshes:
        owners = {b for inf in mesh["influences"] for b, _ in inf}
        if len(owners) == 1 and display_name(scene.models[next(iter(owners))]).startswith("DEF-hand."):
            return mesh, character.bone_index[next(iter(owners))]
    return None

def shorten_held(scene, character, by):
    """--hole-closer: the held mesh's far hole (the soldier's rifle's front
    hand-hole) moves `by` metres nearer its near one, shortening the stretch
    between them, so the support hand reaches it with the mesh carried higher.
    Edits the scene's mesh in place; flat normals are recomputed. Returns
    whether the mesh had two holes."""
    import grip
    found = held_mesh(scene, character)
    if found is None: return False
    mesh, h = found
    bind = scene.bind_globals()
    clip = scene.clips[0]
    first = character.bone_worlds(scene.globals(scene.locals_at(clip, clip["times"][0])))
    points = character.point(Character.bind_points(mesh, bind))
    triangles = points[mesh["corners"].reshape(-1, 3)]
    frame = grip.held_frame(points, character.rest[h][:3, 3], first[h] @ np.linalg.inv(character.rest[h]))
    holes = grip.holes(points, triangles, frame)
    if len(holes) < 2: return False
    moved = grip.shorten_between_holes(points, frame, holes, by)
    # Back to the mesh's own FBX space.
    world = ((moved - character.offset) / character.scale) @ character.linear
    local = (np.c_[world, np.ones(len(world))] @ np.linalg.inv(bind[mesh["model"]]).T)[:, :3]
    p, corners = local, mesh["corners"].reshape(-1, 3)
    normals = np.cross(p[corners[:, 1]] - p[corners[:, 0]], p[corners[:, 2]] - p[corners[:, 0]])
    normals /= np.linalg.norm(normals, axis=1, keepdims=True)
    old = mesh["normals"].reshape(-1, 3, 3)
    flat = (np.einsum("fk,fck->fc", normals, old / np.linalg.norm(old, axis=2, keepdims=True)) > 0.999).mean() > 0.99
    mesh["positions"] = local
    if flat: mesh["normals"] = np.repeat(normals, 3, axis=0).reshape(mesh["normals"].shape)
    return True

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
    parser.add_argument("--lights", help="JSON list of lights the character carries (written to the .vrig): name, position, direction, "
                                         "color, inner, outer, range, lens (see docs/characters.md)")
    parser.add_argument("--pose-frame", type=int, default=0, help="clip frame baked into NAME_pose.vmesh")
    parser.add_argument("--rekey-walk", action="store_true",
                        help="replace the walk clip with a re-keyed one (see walk_cycle.py); keep the original as walk_authored")
    parser.add_argument("--gait", default="", help="re-keyed walk settings, e.g. crouch=0.035,bob=0.02 (walk_cycle.Gait)")
    parser.add_argument("--hole-closer", type=float, default=0.0,
                        help="move the held mesh's far hand-hole this many metres nearer its near one")
    parser.add_argument("--flatten-deltoids", action="store_true",
                        help="flatten the bulge on top of each upper arm near the shoulder to a straight taper")
    parser.add_argument("--shape-arms", action="store_true",
                        help="give each upper arm a tricep and slim each forearm next to the elbow")
    parser.add_argument("--remodel", metavar="MESH",
                        help="model this mesh (the body suit) and the skeleton from scratch, keeping the look (body.py)")
    args = parser.parse_args()
    source = pathlib.Path(args.source); out = pathlib.Path(args.output); out.mkdir(parents=True, exist_ok=True)
    name = args.name or source.stem.lower()
    scene = FbxScene(read_fbx(source))
    filled = fill_unweighted(scene)
    doubled = drop_doubled_faces(scene)
    unfaced = keep_face_weights_on_head(scene)
    character = Character(scene, args.height)
    rebuilt, remapped = None, 0
    if args.remodel:
        import body
        suit = next((m for m in scene.meshes if m["name"] == args.remodel), None)
        hands = next((m for m in scene.meshes if m["name"].lower().startswith("hand")), None)
        if suit is None: sys.exit(f"--remodel: no mesh named {args.remodel}")
        if hands is None: sys.exit("--remodel: no hands mesh for the cuffs to meet")
        bind = scene.bind_globals()
        suit_points = character.point(Character.bind_points(suit, bind))
        skeleton = body.design_skeleton(scene, character, body.Caster(suit_points[suit["corners"].reshape(-1, 3)]))
        original, character = character, Character(scene, args.height, skeleton)
        rebuilt = body.build_suit(scene, character, suit, hands)
        remapped = body.remap_influences(scene, original, skeleton)
    character.add_half_joints()
    evened = even_elbows(scene, character)
    smoothed = smooth_shoulders(scene, character)
    firmed = firm_underarms(scene, character)
    flattened = flatten_deltoids(scene, character) if args.flatten_deltoids else (0, 0.0)
    shaped = shape_arms(scene, character) if args.shape_arms else (0, 0.0)
    if args.hole_closer and not shorten_held(scene, character, args.hole_closer):
        print("  --hole-closer: no mesh held by one hand has two holes; nothing moved")
    overrides = json.loads(pathlib.Path(args.colors).read_text()) if args.colors else {}
    lights = json.loads(pathlib.Path(args.lights).read_text()) if args.lights else []
    glowing = lens_faces(scene, character, lights)
    missing = set()
    def colors(mesh, material, face=None):
        if (id(mesh), face) in glowing: return glowing[(id(mesh), face)]
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
            hands = hand_grips(scene, character, worlds[0], gait.dip, gait.lower, gait.reach)
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
    write_rig(out / f"{name}.vrig", name, source.name, character, clips, lights)
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
    if doubled: print(f"  {doubled} triangles were there twice, facing opposite ways; the inward copies are dropped")
    if rebuilt: print(f"  {rebuilt}; the skeleton is designed anew, {len(character.names)} bones with the helpers")
    if remapped: print(f"  {remapped} vertices of kept meshes moved off dropped bones (face, breast, pelvis) to the bones standing for them")
    if unfaced: print(f"  {unfaced} vertices off the head followed face bones; they follow their strongest other bone instead")
    if evened: print(f"  {evened} sleeve vertices hand over evenly at the elbow")
    if smoothed: print(f"  {smoothed} vertices around the shoulders have smoothed weights")
    if firmed: print(f"  {firmed} vertices under the upper arms follow the arm from closer to the armpit")
    if flattened[0]: print(f"  {flattened[0]} sleeve vertices of the deltoid bulges moved toward the bone, by at most {flattened[1] * 100:.1f} cm")
    if shaped[0]: print(f"  {shaped[0]} sleeve vertices shaped into triceps and slimmer forearms, by at most {shaped[1] * 100:.1f} cm")
    for note in notes: print("  " + note)
    authored = next((c for c in clips if c["name"] != "walk" or not args.rekey_walk), None)
    if character.skeleton is None: report_fidelity(scene, character, authored["worlds"] if authored else [])

if __name__ == "__main__":
    main()
