"""Model a character's body and skeleton from scratch, keeping his look.

The soldier's suit (the `Armor` mesh) came out of Blender as a plain tube
round each limb, with no edge loops at the joints, weights spread across them
and the arm bones sitting 1-3 cm off the sleeves' centres, so a folded elbow
pinched and swelled whatever the weights did. This module replaces both the
suit and the rig:

- `design_skeleton` builds a clean biped skeleton: the spine, collarbones,
  arms, hands and legs of a Rigify rig (same names, so the walk, the grips and
  the tests keep working) with no face or breast bones, the arm joints at the
  centres of the new sleeves, and bone frames pointing along the bones.
- `build_suit` sweeps rings of vertices along the bones: a ring at every
  joint (five across each elbow, one per quarter-turn helper), designed
  outlines for the chest, shoulders and arms (a superelliptic armour plate,
  deltoids, a tricep, forearms no wider than the elbow, cuffs that meet the
  hands), and the legs, hips and hood copied from the original's outlines.
  Weights are set ring by ring.
- `retarget` carries a clip authored for the original rig onto the new one:
  each bone turns as its original did, and joints stay at their new lengths.

numpy only; import_model.py drives it with --remodel.
"""
import numpy as np

SECTORS = 16            # vertices round a limb ring
TORSO_SECTORS = 24      # round the torso and head
UP, FRONT, LEFT = np.array([0, 1.0, 0]), np.array([0, 0, 1.0]), np.array([1.0, 0, 0])

def _unit(v):
    return v / np.linalg.norm(v)

def _smoothstep(x):
    x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)

def _rotation_part(m):
    return m[:3, :3] / np.cbrt(abs(np.linalg.det(m[:3, :3])))

# --- Sampling the original ------------------------------------------------------

class Caster:
    """Rays against a triangle soup, one ray at a time (vectorised over triangles)."""
    def __init__(self, triangles):
        self.a = triangles[:, 0]; self.e1 = triangles[:, 1] - self.a; self.e2 = triangles[:, 2] - self.a

    def hit(self, origin, direction, limit):
        h = np.cross(direction, self.e2); det = np.einsum("ij,ij->i", self.e1, h)
        ok = np.abs(det) > 1e-12; safe = np.where(ok, det, 1)
        s = origin - self.a; u = np.einsum("ij,ij->i", s, h) / safe
        q = np.cross(s, self.e1); v = np.einsum("j,ij->i", direction, q) / safe
        t = np.einsum("ij,ij->i", self.e2, q) / safe
        hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-4) & (t < limit)
        return t[hit].min() if hit.any() else np.nan

class Shape:
    """A ring's outline as low Fourier modes of its radius round the ring:
    mode 0 is its size, mode 1 its centre's offset from the axis, mode 2 how
    elliptical it is, 3 and 4 how boxy. Fitted to radii, missing ones left out."""
    HARMONICS = 4
    MODES = 1 + 2 * HARMONICS

    def __init__(self, coefficients):
        self.c = np.asarray(coefficients, float)

    @staticmethod
    def basis(angles):
        columns = [np.ones_like(angles)]
        for k in range(1, Shape.HARMONICS + 1): columns += [np.cos(k * angles), np.sin(k * angles)]
        return np.stack(columns, axis=1)

    @classmethod
    def fit(cls, radii):
        angles = np.linspace(0, 2 * np.pi, len(radii), endpoint=False)
        good = np.isfinite(radii)
        if good.sum() < cls.MODES + 3: return None
        return cls(np.linalg.lstsq(cls.basis(angles[good]), radii[good], rcond=None)[0])

    def radii(self, count):
        return self.basis(np.linspace(0, 2 * np.pi, count, endpoint=False)) @ self.c

    def centre(self):
        """The outline's centre, offset from the axis, in the ring's (u, v) frame."""
        return np.array([self.c[1], self.c[2]])

def frame(axis, up):
    """Unit vectors across `axis`: `up` made perpendicular, and the third."""
    u = _unit(up - (up @ axis) * axis)
    return u, np.cross(axis, u)

def sample(caster, origin, axis, up, sectors, limit):
    """The original surface round `origin`, perpendicular to `axis`: a Shape,
    or None where too few rays meet it. Rays far longer than the ring's
    median (another limb, in the T-pose) are left out."""
    u, v = frame(axis, up)
    angles = np.linspace(0, 2 * np.pi, sectors * 2, endpoint=False)   # twice the vertices: a steadier fit
    radii = np.array([caster.hit(origin, np.cos(a) * u + np.sin(a) * v, limit) for a in angles])
    if np.isfinite(radii).sum() >= 6:
        radii[radii > 1.6 * np.nanmedian(radii)] = np.nan
    return Shape.fit(radii)

def superellipse(width, depth, exponent, centre, angles):
    """Radii round a ring for a superellipse |x/width|^n + |y/depth|^n = 1 whose
    centre sits `centre` (u, v) off the axis: u is the ring's up (or front), v
    its side. Width is along v, depth along u."""
    t = np.linspace(0, 2 * np.pi, 720, endpoint=False)
    sgn = lambda x: np.sign(x) * np.abs(x) ** (2 / exponent)
    pu = depth * sgn(np.cos(t)) + centre[0]; pv = width * sgn(np.sin(t)) + centre[1]
    polar = np.arctan2(pv, pu) % (2 * np.pi); radius = np.hypot(pu, pv)
    order = np.argsort(polar)
    return np.interp(angles % (2 * np.pi), polar[order], radius[order], period=2 * np.pi)

# --- The skeleton -------------------------------------------------------------

class Skeleton:
    """A character's bones by design: FBX twins in order (parents first), their
    parents' indices, rest worlds in engine space, and names; `original` is
    the Character read from the FBX. Every bone answers to an original one,
    which is what lets clips be retargeted and kept meshes keep their
    weights."""
    def __init__(self, original, deforming, parents, rest, names):
        self.original, self.deforming, self.parents, self.rest, self.names = original, deforming, parents, rest, names
        self.index = {n: k for k, n in enumerate(names)}

FACE_PARTS = ("brow", "cheek", "chin", "ear", "eye", "forehead", "jaw", "lid", "lip", "nose", "teeth", "temple", "tongue")
DROPPED = {"face": lambda n: n.removeprefix("DEF-").split(".")[0] in FACE_PARTS,
           "breast": lambda n: n.startswith("DEF-breast"),
           "pelvis": lambda n: n.startswith("DEF-pelvis")}
STAND_IN = {"face": "DEF-spine.006", "breast": "DEF-spine.003", "pelvis": "DEF-spine"}

def bone_frame(head, tail, up=UP):
    """A bone's rest world at `head`, its own Y along the bone toward `tail`,
    its Z as near `up` as the bone allows."""
    y = _unit(tail - head)
    z = up - (up @ y) * y
    if np.linalg.norm(z) < 1e-6: z = FRONT - (FRONT @ y) * y
    z = _unit(z); x = np.cross(y, z)
    m = np.eye(4); m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = x, y, z, head
    return m

def design_skeleton(scene, fbx, caster):
    """The new skeleton from the original rig's landmarks and the original
    suit (`caster`): the spine, legs, hands and fingers keep their joints and
    frames (their meshes are kept); each collarbone ends at, and each arm
    bone runs between, the centres of the sleeve's cross-sections there; the
    face, breast and pelvis bones are gone."""
    from import_model import display_name
    names = {b: display_name(scene.models[b]) for b in fbx.deforming}
    kept = [b for b in fbx.deforming if not any(drop(names[b]) for drop in DROPPED.values())]
    heads = {names[b]: fbx.rest[fbx.bone_index[b]][:3, 3].copy() for b in kept}
    rest = {names[b]: fbx.rest[fbx.bone_index[b]].copy() for b in kept}
    # Arm joints at the sleeve's centre: the original bone ran 1-3 cm forward and up of it.
    def centre_near(point, axis, up, span=0.03):
        u, v = frame(axis, up); offsets = []
        for d in (-span, 0.0, span):
            shape = sample(caster, point + axis * d, axis, up, SECTORS, 0.3)
            if shape is not None: offsets.append(u * shape.centre()[0] + v * shape.centre()[1])
        return point + (np.mean(offsets, axis=0) if offsets else 0.0)
    for side in "LR":
        s, e, w = heads[f"DEF-upper_arm.{side}"], heads[f"DEF-forearm.{side}"], heads[f"DEF-hand.{side}"]
        upper_axis = _unit(e - s)
        shoulder = centre_near(s + upper_axis * .04, upper_axis, UP) - upper_axis * .04
        elbow = centre_near(e, upper_axis, UP)
        heads[f"DEF-upper_arm.{side}"], heads[f"DEF-forearm.{side}"] = shoulder, elbow
        heads[f"DEF-upper_arm.{side}.001"] = (shoulder + elbow) / 2
        heads[f"DEF-forearm.{side}.001"] = (elbow + w) / 2
        rest[f"DEF-shoulder.{side}"] = bone_frame(heads[f"DEF-shoulder.{side}"], shoulder)
        for name, tail in ((f"DEF-upper_arm.{side}", elbow), (f"DEF-upper_arm.{side}.001", elbow),
                           (f"DEF-forearm.{side}", w), (f"DEF-forearm.{side}.001", w)):
            rest[name] = bone_frame(heads[name], tail)
    parent_name = {}
    for b in kept:
        p = fbx.parents[fbx.bone_index[b]]
        while p > 0 and fbx.names[p] not in rest:          # a dropped parent: the bone above it
            p = fbx.parents[p]
        parent_name[names[b]] = fbx.names[p] if p > 0 else None
    order = sorted(kept, key=lambda b: fbx.bone_index[b])   # the original's order: parents first
    index = {names[b]: k + 1 for k, b in enumerate(order)}
    return Skeleton(fbx, order, [-1] + [index.get(parent_name[names[b]], 0) for b in order],
                    [np.eye(4)] + [rest[names[b]] for b in order], ["root"] + [names[b] for b in order])

def remap_influences(scene, fbx, skeleton):
    """Kept meshes' weights on bones the skeleton dropped go to the bone that
    stands for them (STAND_IN). Returns the vertices changed."""
    from import_model import display_name
    names = {b: display_name(scene.models[b]) for b in fbx.deforming}
    twins = {names[b]: b for b in skeleton.deforming}
    stand_in = {b: twins[STAND_IN[what]] for b, n in names.items() for what, drop in DROPPED.items() if drop(n)}
    changed = 0
    for mesh in scene.meshes:
        if mesh.get("generated"): continue
        for v, inf in enumerate(mesh["influences"]):
            if not any(b in stand_in for b, _ in inf): continue
            merged = {}
            for b, w in inf: merged[stand_in.get(b, b)] = merged.get(stand_in.get(b, b), 0.0) + w
            mesh["influences"][v] = list(merged.items()); changed += 1
    return changed

def retarget(skeleton, fbx, globals_, world):
    """Bone worlds of a clip frame (FBX globals) on the skeleton: each bone
    turns as its original did (its world rotation relative to its rest), the
    hips keep the original's place, and every other joint hangs from its
    parent at its rest offset, so no bone stretches. Scales are 1."""
    worlds = [np.eye(4)]
    for k, b in enumerate(skeleton.deforming, start=1):
        original = world(globals_[b])
        turn = _rotation_part(original) @ _rotation_part(fbx.rest[fbx.bone_index[b]]).T
        m = np.eye(4); m[:3, :3] = turn @ _rotation_part(skeleton.rest[k])
        parent = skeleton.parents[k]
        if parent <= 0: m[:3, 3] = original[:3, 3]
        else: m[:3, 3] = (worlds[parent] @ (np.linalg.inv(skeleton.rest[parent]) @ skeleton.rest[k][:, 3]))[:3]
        worlds.append(m)
    return worlds

# --- Sweeping the suit -----------------------------------------------------------

class Station:
    """One ring to sweep: where, facing which way, weighted how. `shape` is a
    Shape to copy (sampled from the original), `radii` a designed outline, or
    neither to take a neighbour's outline times `scale`."""
    def __init__(self, point, axis, weight, shape=None, radii=None, scale=1.0):
        self.point, self.axis, self.weight = point, axis, weight
        self.shape, self.radii, self.scale = shape, radii, scale

class Sweep:
    """Vertices, faces and weights of a surface swept ring by ring."""
    def __init__(self):
        self.points, self.weights, self.centres, self.faces = [], [], [], []
        self.rings = []   # (first vertex, count, centre)
        self.caps = {}    # ring index -> its cap's apex vertex

    def ring(self, centre, axis, up, radii, weight):
        """Adds a ring of len(radii) vertices round `centre`, perpendicular to `axis`; returns its index."""
        u, v = frame(axis, up)
        angles = np.linspace(0, 2 * np.pi, len(radii), endpoint=False)
        start = len(self.points)
        for a, r in zip(angles, radii):
            self.points.append(centre + r * (np.cos(a) * u + np.sin(a) * v))
            self.weights.append(dict(weight)); self.centres.append(centre)
        self.rings.append((start, len(radii), centre))
        return len(self.rings) - 1

    def join(self, a, b):
        """Quads between two rings of equal size, as triangles."""
        (sa, n, _), (sb, m, _) = self.rings[a], self.rings[b]
        assert n == m
        for k in range(n):
            p, q, r, s = sa + k, sa + (k + 1) % n, sb + k, sb + (k + 1) % n
            self.faces.append((p, q, s)); self.faces.append((p, s, r))

    def cap(self, ring, apex, weight):
        """Closes a ring with a fan to one vertex at `apex`."""
        s, n, centre = self.rings[ring]
        c = len(self.points)
        self.points.append(np.asarray(apex, float)); self.weights.append(dict(weight)); self.centres.append(centre)
        self.caps[ring] = c
        for k in range(n): self.faces.append((s + k, s + (k + 1) % n, c))

    def orient(self):
        """Faces wound to face away from their rings' centres."""
        P, C, F = np.array(self.points), np.array(self.centres), np.array(self.faces)
        normals = np.cross(P[F[:, 1]] - P[F[:, 0]], P[F[:, 2]] - P[F[:, 0]])
        outward = P[F].mean(1) - C[F].mean(1)
        flip = np.einsum("ij,ij->i", normals, outward) < 0
        F[flip] = F[flip][:, [0, 2, 1]]
        self.faces = [tuple(f) for f in F]

def _blend(weights):
    total = sum(weights.values())
    return {b: w / total for b, w in weights.items() if w / total > 1e-3}

class Suit:
    def __init__(self, character, caster, bone_ids):
        self.character, self.caster, self.bone = character, caster, bone_ids
        self.rest = {n: character.rest[k][:3, 3] for k, n in enumerate(character.names)}
        self.sweep = Sweep()

    def chain(self, stations, up, sectors, closed=(), smooth=(0.15, 0.3, 0.3, 0.2, 0.2), shape=None):
        """Rings along `stations`: sampled or designed outlines as Fourier
        modes, smoothed along the chain by `smooth` (size, centre, ellipse,
        boxiness) so copied and designed stretches meet without a step.
        Stations with neither take their nearest neighbour's outline, times
        their `scale`. `shape` (station, radii) -> radii can reshape a ring
        and move its station. `closed` caps "start" and/or "end" 2 cm beyond
        the ring. Returns the ring indices."""
        shapes = [s.shape if s.shape is not None else Shape.fit(s.radii) if s.radii is not None else None for s in stations]
        known = [i for i, s in enumerate(shapes) if s is not None]
        if not known: raise ValueError("a chain has no outline anywhere")
        coefficients = np.array([(shapes[i] if shapes[i] is not None else shapes[min(known, key=lambda k: abs(k - i))]).c
                                 for i in range(len(stations))])
        for mode in range(Shape.MODES):
            weight = smooth[min((mode + 1) // 2, len(smooth) - 1)]
            column = coefficients[:, mode].copy()
            for _ in range(3):
                column[1:-1] = column[1:-1] * (1 - weight) + (column[:-2] + column[2:]) * weight / 2
            coefficients[:, mode] = column
        rings = []
        for s, c in zip(stations, coefficients):
            radii = Shape(c).radii(sectors) * s.scale
            if shape is not None: radii = shape(s, radii)
            rings.append(self.sweep.ring(s.point, s.axis, up, radii, _blend(s.weight)))
        for a, b in zip(rings, rings[1:]): self.sweep.join(a, b)
        if "start" in closed: self.sweep.cap(rings[0], stations[0].point - stations[0].axis * .02, _blend(stations[0].weight))
        if "end" in closed: self.sweep.cap(rings[-1], stations[-1].point + stations[-1].axis * .02, _blend(stations[-1].weight))
        return rings

    def limb(self, joints, bones, spacing, joint_rings, up, sectors, closed=(), before=0.0, after=0.0,
             outline=None, shape=None, limit=0.5):
        """A limb of several stretches: `joints` [j0, j1, ...] and per stretch
        `bones` (first, second), the second taking over from the first by a
        smoothstep across the stretch's middle third. Rings sit every
        `spacing` metres, plus `joint_rings` at each inner joint: (offset in
        metres, share of the stretch above) pairs, at the joint's bisector.
        `before` extends the start inside what carries the limb, `after` the
        end. `outline` (stretch, metres along it, point, axis) -> radii
        designs a ring; without it, rings copy the original."""
        stations = []
        last_stretch = len(joints) - 2
        for k in range(last_stretch + 1):
            a, b = joints[k], joints[k + 1]; axis = _unit(b - a); length = np.linalg.norm(b - a)
            first_bone, second_bone = bones[k]
            def station(t, point=None, direction=None):
                share = _smoothstep((t / length - .5) / .34 + .5)
                point = a + axis * t if point is None else point; direction = axis if direction is None else direction
                s = Station(point, direction, {first_bone: 1 - share, second_bone: share})
                if t < 0:   # inside what carries the limb, the rings narrow to a point, so they stay hidden
                    s.scale = max(1 + t / before * .8, .2) if before else 1.0
                elif outline is not None: s.radii = outline(k, t, point, direction)
                else: s.shape = sample(self.caster, point, direction, up, sectors, limit)
                return s
            first = -before if k == 0 else max(o for o, _ in joint_rings[k - 1]) + spacing
            last = length + after if k == last_stretch else length + min(o for o, _ in joint_rings[k]) - spacing
            count = max(int(round((last - first) / spacing)), 1)
            for t in np.linspace(first, last, count + 1): stations.append(station(t))
            if k < last_stretch:
                following = _unit(joints[k + 2] - b)
                for offset, share in sorted(joint_rings[k]):
                    direction = _unit(axis + following) if offset == 0 else (axis if offset < 0 else following)
                    s = station(length + min(offset, 0.0), b + direction * offset, direction)
                    s.weight = {second_bone: share, bones[k + 1][0]: 1 - share}
                    stations.append(s)
        return self.chain(stations, up, sectors, closed=closed, shape=shape)

# --- The soldier's suit ------------------------------------------------------------

def arm_outline(joints, cuff):
    """The designed arm, as radii round a station: a deltoid cap at the
    shoulder, an upper arm slightly taller than deep with a tricep on its
    back and underside, an elbow that the pad covers, and a forearm no wider
    than the elbow, tapering to a cuff that meets the hand (`cuff`: centre
    and radius of the hand's wrist ring)."""
    shoulder, elbow, wrist = joints
    upper_axis = _unit(elbow - shoulder)
    upper_length, fore_length = np.linalg.norm(elbow - shoulder), np.linalg.norm(wrist - elbow)
    tricep_dir = _unit(np.array([0.0, -1.0, -1.0]) - (np.array([0.0, -1.0, -1.0]) @ upper_axis) * upper_axis)
    angles = np.linspace(0, 2 * np.pi, SECTORS, endpoint=False)
    def outline(stretch, t, point, direction):
        u, v = frame(direction, UP)
        if stretch == 0:
            s = t / upper_length
            # Size along the upper arm: deltoid 9.2 cm, 8.4 at the middle, 6.3 at the elbow.
            size = np.interp(s, [0.0, .15, .35, .55, .8, 1.0], [.090, .092, .087, .080, .070, .063])
            tall = 1.04 - .04 * _smoothstep((s - .6) / .4); deep = 2 - tall
            radii = superellipse(size * deep, size * tall, 2.1, (0.0, 0.0), angles)
            # The tricep: up to 1.2 cm on the back and underside, most at 55 % of the way down.
            facing = np.cos(angles) * (u @ tricep_dir) + np.sin(angles) * (v @ tricep_dir)
            radii = radii + .012 * np.sin(np.pi * np.clip((s - .12) / .78, 0, 1)) * _smoothstep((facing - .1) / .6)
        else:
            f = t / fore_length
            # The forearm: the elbow's size for 6 cm, then a taper to the cuff.
            size = np.interp(f, [0.0, .16, .35, .55, .75, .9, 1.0], [.063, .062, .058, .053, .048, .045, cuff[1] + .002])
            tall = 1.05 - .05 * _smoothstep((f - .3) / .6); deep = 2 - tall
            radii = superellipse(size * deep, size * tall, 2.1, (0.0, 0.0), angles)
        # Under the elbow pad, the sleeve sits 6 % closer to the bone, so the pad covers it as it folds.
        if np.linalg.norm(point - elbow) <= .065: radii = radii * .94
        return radii
    return outline

def build_suit(scene, character, mesh, hands):
    """Replaces `mesh`'s geometry and weights with the suit swept along the
    character's new bones (see the module docstring). `hands` is the hands
    mesh, whose wrist rings the cuffs meet. Returns a note."""
    from import_model import Character, display_name
    bind = scene.bind_globals()
    points = character.point(Character.bind_points(mesh, bind))
    caster = Caster(points[mesh["corners"].reshape(-1, 3)])
    bone = {display_name(scene.models[b]): b for b in character.deforming}
    suit = Suit(character, caster, bone)
    R = suit.rest
    # --- Legs: from inside the pelvis to the ankle (inside the boot), copied from the original, one loop at the knee.
    for side in "LR":
        joints = [R[f"DEF-thigh.{side}"], R[f"DEF-shin.{side}"], R[f"DEF-foot.{side}"]]
        suit.limb(joints, [(bone[f"DEF-thigh.{side}"], bone[f"DEF-thigh.{side}.001"]), (bone[f"DEF-shin.{side}"], bone[f"DEF-shin.{side}.001"])],
                  spacing=.045, joint_rings=[[(-.04, 1.0), (0.0, .5), (.04, 0.0)]], up=FRONT, sectors=SECTORS,
                  closed=("end",), before=.05, after=-.03)
    # --- Torso, neck and hood: one sweep from the crotch to the crown. The hips (under the belt) and
    # the neck and hood copy the original; between them the chest and shoulders are designed: a
    # superelliptic armour plate, deeper than wide, spreading into the shoulders and narrowing to the neck.
    stations = []
    heads = [R[f"DEF-spine{s}"] for s in ("", ".001", ".002", ".003", ".004", ".005", ".006")]
    bones = [bone[f"DEF-spine{s}"] for s in ("", ".001", ".002", ".003", ".004", ".005", ".006")]
    path = heads + [heads[6] + UP * .155]
    angles = np.linspace(0, 2 * np.pi, TORSO_SECTORS, endpoint=False)
    # height: half-width, front and back depth from the spine line, boxiness
    plate = np.array([[1.00, .157, .194, .150, 2.4], [1.08, .158, .198, .160, 2.5], [1.16, .159, .198, .166, 2.6],
                      [1.24, .164, .196, .168, 2.6], [1.30, .185, .188, .167, 2.5], [1.36, .205, .180, .162, 2.3],
                      [1.42, .170, .165, .150, 2.2], [1.46, .125, .140, .125, 2.1], [1.50, .090, .105, .096, 2.0]])
    for k in range(len(path) - 1):
        a, b = path[k], path[k + 1]; axis = _unit(b - a); length = np.linalg.norm(b - a)
        spacing = .03 if k < 4 else .02
        count = max(int(round(length / spacing)), 1)
        for t in np.linspace(-.05 if k == 0 else 0.0, length, count + 1)[:-1 if k < len(path) - 2 else None]:
            previous = .5 * (1 - _smoothstep(t / (length / 2))) if k else 0.0
            following = .5 * _smoothstep((t - length / 2) / (length / 2)) if k < 6 else 0.0
            weight = {bones[k]: 1 - previous - following}
            if previous: weight[bones[k - 1]] = previous
            if following: weight[bones[k + 1]] = following
            point = a + axis * t
            station = Station(point, axis, weight)
            if plate[0, 0] <= point[1] <= plate[-1, 0]:
                width, front, back, boxiness = (np.interp(point[1], plate[:, 0], plate[:, c]) for c in range(1, 5))
                station.radii = superellipse(width, (front + back) / 2, boxiness, ((front - back) / 2, 0.0), angles)
            else:
                station.shape = sample(caster, point, axis, FRONT, TORSO_SECTORS, .3)
            stations.append(station)
    suit.chain(stations, FRONT, TORSO_SECTORS, closed=("start", "end"))
    # --- Arms: from inside the torso through the deltoid, five loops across the elbow, to the cuff.
    hand_points = character.point(Character.bind_points(hands, bind))
    for side in "LR":
        joints = [R[f"DEF-upper_arm.{side}"], R[f"DEF-forearm.{side}"], R[f"DEF-hand.{side}"]]
        fore_axis = _unit(joints[2] - joints[1])
        ring = hand_points[np.abs((hand_points - joints[2]) @ fore_axis) < .012]
        centre = ring.mean(0)
        cuff = (centre, np.linalg.norm((ring - centre) - np.outer((ring - centre) @ fore_axis, fore_axis), axis=1).max())
        outline = arm_outline(joints, cuff)
        # The last 8 cm of the forearm slide over to the cuff's centre, so the sleeve meets the hand.
        shift, fore_length = cuff[0] - joints[2], np.linalg.norm(joints[2] - joints[1])
        def cuffed(station, radii, elbow=joints[1], fore_axis=fore_axis, shift=shift, fore_length=fore_length):
            along = (station.point - elbow) @ fore_axis
            station.point = station.point + shift * _smoothstep((along - (fore_length - .08)) / .08)
            return radii
        rings = suit.limb(joints, [(bone[f"DEF-upper_arm.{side}"], bone[f"DEF-upper_arm.{side}.001"]), (bone[f"DEF-forearm.{side}"], bone[f"DEF-forearm.{side}.001"])],
                          spacing=.03, joint_rings=[[(-.06, 1.0), (-.03, .75), (0.0, .5), (.03, .25), (.06, 0.0)]], up=UP, sectors=SECTORS,
                          closed=("start", "end"), before=.08, after=0.0, outline=outline, shape=cuffed)
        # The shoulder: the rings inside the torso belong to the collarbone; the arm takes over by 6 cm out.
        collar, arm = bone[f"DEF-shoulder.{side}"], bone[f"DEF-upper_arm.{side}"]
        axis = _unit(joints[1] - joints[0])
        for r in rings:
            start, count, centre = suit.sweep.rings[r]
            along = (centre - joints[0]) @ axis
            if along > .09: break
            share = _smoothstep((along + .02) / .08)
            for v in range(start, start + count): suit.sweep.weights[v] = _blend({collar: 1 - share, arm: share})
        suit.sweep.weights[suit.sweep.caps[rings[0]]] = {collar: 1.0}
    suit.sweep.orient()
    # --- Into the mesh, in its own FBX space, with smooth normals.
    P = np.array(suit.sweep.points); F = np.array(suit.sweep.faces)
    face_normals = np.cross(P[F[:, 1]] - P[F[:, 0]], P[F[:, 2]] - P[F[:, 0]])
    normals = np.zeros_like(P)
    for c in range(3): np.add.at(normals, F[:, c], face_normals)
    normals /= np.linalg.norm(normals, axis=1, keepdims=True) + 1e-12
    world = ((P - character.offset) / character.scale) @ character.linear
    local = (np.c_[world, np.ones(len(world))] @ np.linalg.inv(bind[mesh["model"]]).T)[:, :3]
    local_normals = (normals @ character.linear) @ bind[mesh["model"]][:3, :3]
    local_normals /= np.linalg.norm(local_normals, axis=1, keepdims=True) + 1e-12
    mesh["positions"] = local
    mesh["corners"] = F.reshape(-1)
    mesh["normals"] = local_normals[F.reshape(-1)]
    mesh["face_material"] = np.zeros(len(F), int)
    mesh["influences"] = [[(b, w) for b, w in weights.items()] for weights in suit.sweep.weights]
    mesh["generated"] = True
    return f"{mesh['name']} remodelled: {len(P)} vertices, {len(F)} triangles"
