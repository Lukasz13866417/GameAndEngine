"""Remodel a character's body suit as a ring sweep along its bones.

The soldier's suit (the `Armor` mesh) is a plain tube around each limb, with
no edge loops at the joints and skin weights that spread across them, so a
folded elbow pinched and swelled. This rebuilds the suit from scratch: rings
of vertices swept along the bones, a dedicated ring at every joint (and one
either side of an elbow, for its quarter helpers), and weights assigned ring
by ring. Each ring copies its size, shape and centre from the original suit,
sampled by rays cast from the bone, so he looks like the current soldier.
numpy only; import_model.py calls `rebuild_suit`.
"""
import numpy as np

SECTORS = 16            # vertices round a limb ring
TORSO_SECTORS = 24      # round the torso and head

def _unit(v):
    return v / np.linalg.norm(v)

def _smoothstep(x):
    x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)

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
    elliptical it is. Fitted to sampled radii, missing ones left out."""
    MODES = 5   # a0, a1, b1, a2, b2

    def __init__(self, coefficients):
        self.c = np.asarray(coefficients, float)

    @classmethod
    def fit(cls, radii):
        angles = np.linspace(0, 2 * np.pi, len(radii), endpoint=False)
        good = np.isfinite(radii)
        if good.sum() < 6: return None
        basis = np.c_[np.ones(good.sum()), np.cos(angles[good]), np.sin(angles[good]), np.cos(2 * angles[good]), np.sin(2 * angles[good])]
        return cls(np.linalg.lstsq(basis, radii[good], rcond=None)[0])

    def radii(self, count):
        angles = np.linspace(0, 2 * np.pi, count, endpoint=False)
        return (self.c[0] + self.c[1] * np.cos(angles) + self.c[2] * np.sin(angles)
                + self.c[3] * np.cos(2 * angles) + self.c[4] * np.sin(2 * angles))

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

class Station:
    """One ring to sweep: where, facing which way, weighted how; `sampled`
    false where the original is not worth copying (inside another part),
    where the ring takes its neighbour's outline times `scale`."""
    def __init__(self, point, axis, weight, sampled=True, scale=1.0):
        self.point, self.axis, self.weight, self.sampled, self.scale = point, axis, weight, sampled, scale

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

    def chain(self, stations, up, sectors, limit=0.5, closed=(), smooth=(0.15, 0.3, 0.3), shape=None):
        """Rings along `stations`, each copying the original's outline there,
        with sizes (mode 0), centres (modes 1-2) and ellipticity (3-4)
        smoothed along the chain by `smooth`. Unsampled stations take their
        nearest sampled neighbour's outline, scaled by their `scale`. `shape`
        (station, radii) -> radii can reshape a ring. `closed` caps "start"
        and/or "end" 2 cm beyond the ring. Returns the ring indices."""
        shapes = [sample(self.caster, s.point, s.axis, up, sectors, limit) if s.sampled else None for s in stations]
        known = [i for i, s in enumerate(shapes) if s is not None]
        if not known: raise ValueError("the original mesh was not found along a chain")
        coefficients = np.array([(shapes[i] if shapes[i] is not None else shapes[min(known, key=lambda k: abs(k - i))]).c
                                 for i in range(len(stations))])
        for mode in range(Shape.MODES):
            weight = smooth[min(mode, 2) if mode < 3 else 2]
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

    def limb(self, joints, bones, spacing, joint_rings, up, sectors, closed=(), before=0.0, after=0.0, shape=None):
        """A limb of several stretches: `joints` [j0, j1, ...] and per stretch
        `bones` (first, second), the second taking over from the first by a
        smoothstep across the stretch's middle third. Rings sit every
        `spacing` metres, plus `joint_rings` at each inner joint: (offset in
        metres, share of the stretch above) pairs, at the joint's bisector.
        `before` extends the start inside what carries the limb (unsampled),
        `after` the end."""
        stations = []
        last_stretch = len(joints) - 2
        for k in range(last_stretch + 1):
            a, b = joints[k], joints[k + 1]; axis = _unit(b - a); length = np.linalg.norm(b - a)
            first_bone, second_bone = bones[k]
            def station(t):
                share = _smoothstep((t / length - .5) / .34 + .5)
                # Inside what carries the limb, the rings narrow to a point, so they stay hidden.
                return Station(a + axis * t, axis, {first_bone: 1 - share, second_bone: share}, sampled=t >= 0,
                               scale=1.0 if t >= 0 or not before else max(1 + t / before * .8, .2))
            first = -before if k == 0 else max(o for o, _ in joint_rings[k - 1]) + spacing
            last = length + after if k == last_stretch else length + min(o for o, _ in joint_rings[k]) - spacing
            count = max(int(round((last - first) / spacing)), 1)
            for t in np.linspace(first, last, count + 1): stations.append(station(t))
            if k < last_stretch:
                following = _unit(joints[k + 2] - b)
                for offset, share in sorted(joint_rings[k]):
                    direction = _unit(axis + following) if offset == 0 else (axis if offset < 0 else following)
                    stations.append(Station(b + direction * offset, direction, {second_bone: share, bones[k + 1][0]: 1 - share}))
        return self.chain(stations, up, sectors, closed=closed, shape=shape)

def rebuild_suit(scene, character, mesh):
    """Replaces `mesh`'s geometry and weights with a ring sweep along the
    character's bones (see the module docstring). Returns a note."""
    from import_model import Character, display_name
    bind = scene.bind_globals()
    points = character.point(Character.bind_points(mesh, bind))
    caster = Caster(points[mesh["corners"].reshape(-1, 3)])
    bone = {display_name(scene.models[b]): b for b in character.deforming}
    suit = Suit(character, caster, bone)
    R = suit.rest
    up, front = np.array([0, 1.0, 0]), np.array([0, 0, 1.0])
    # --- Legs: from inside the pelvis to the ankle (inside the boot), one loop at the knee.
    for side in "LR":
        joints = [R[f"DEF-thigh.{side}"], R[f"DEF-shin.{side}"], R[f"DEF-foot.{side}"]]
        suit.limb(joints, [(bone[f"DEF-thigh.{side}"], bone[f"DEF-thigh.{side}.001"]), (bone[f"DEF-shin.{side}"], bone[f"DEF-shin.{side}.001"])],
                  spacing=.045, joint_rings=[[(-.04, 1.0), (0.0, .5), (.04, 0.0)]], up=front, sectors=SECTORS,
                  closed=("end",), before=.05, after=-.03)
    # --- Torso, neck and hood: one sweep from the crotch to the top of the head. A vertex at a
    # bone's head is shared with the bone before; a bone owns the middle of its stretch.
    stations = []
    heads = [R[f"DEF-spine{s}"] for s in ("", ".001", ".002", ".003", ".004", ".005", ".006")]
    bones = [bone[f"DEF-spine{s}"] for s in ("", ".001", ".002", ".003", ".004", ".005", ".006")]
    path = heads + [heads[6] + up * .155]
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
            stations.append(Station(a + axis * t, axis, weight))
    suit.chain(stations, front, TORSO_SECTORS, limit=.3, closed=("start", "end"))
    # --- Arms: from inside the torso through the shoulder, loops at the elbow for its quarter helpers, to the wrist.
    for side in "LR":
        joints = [R[f"DEF-upper_arm.{side}"], R[f"DEF-forearm.{side}"], R[f"DEF-hand.{side}"]]
        # A tricep: the back and underside of the upper arm swell by up to 1.5 cm, most midway along.
        axis = _unit(joints[1] - joints[0]); length = np.linalg.norm(joints[1] - joints[0])
        back = _unit(np.array([0.0, -1.0, -1.0]) - (np.array([0.0, -1.0, -1.0]) @ axis) * axis)
        u, v = frame(axis, up)
        def tricep(station, radii):
            along = (station.point - joints[0]) @ axis
            # Under the elbow pad (a separate mesh), the sleeve sits 6% closer to the bone, so the pad covers it as it folds.
            if abs(along - length) <= .065: radii = radii * .94
            if not (0 < along < length - .03) or station.axis @ axis < .99: return radii
            angles = np.linspace(0, 2 * np.pi, len(radii), endpoint=False)
            facing = np.cos(angles) * (u @ back) + np.sin(angles) * (v @ back)
            return radii + .015 * np.sin(np.pi * np.clip((along - .04) / (length - .07), 0, 1)) * _smoothstep((facing - .1) / .6)
        rings = suit.limb(joints, [(bone[f"DEF-upper_arm.{side}"], bone[f"DEF-upper_arm.{side}.001"]), (bone[f"DEF-forearm.{side}"], bone[f"DEF-forearm.{side}.001"])],
                          spacing=.03, joint_rings=[[(-.06, 1.0), (-.03, .75), (0.0, .5), (.03, .25), (.06, 0.0)]], up=up, sectors=SECTORS,
                          closed=("start", "end"), before=.06, after=-.01, shape=tricep)
        # The shoulder: the rings inside the torso belong to the collarbone, the arm takes over by 6 cm out.
        collar, arm = bone[f"DEF-shoulder.{side}"], bone[f"DEF-upper_arm.{side}"]
        axis = _unit(joints[1] - joints[0])
        for ring in rings:
            start, count, centre = suit.sweep.rings[ring]
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
    return f"{mesh['name']} remodelled as a sweep: {len(P)} vertices, {len(F)} triangles"
