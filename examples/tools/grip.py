"""Close a character's fingers on something it holds.

Every finger (thumb, index, middle, ring, pinky: three DEF bones each) curls
about its own bend axis from the modelled rest hand, until its vertices would
touch the held mesh. One curl amount per finger scales three joint angles;
bisection finds the largest that stays a couple of millimetres outside.
The other hand can support the held mesh from below (support_placement).
Everything is computed in bind space: a held mesh bound to one hand moves
rigidly with it, so the grip holds in every frame. Needs numpy only.
"""
import numpy as np

FINGERS = ("thumb", "f_index", "f_middle", "f_ring", "f_pinky")
# Most each joint may bend (degrees) for a full curl: knuckle, middle, tip.
MAX_BEND = {"thumb": (25.0, 35.0, 35.0), "f_index": (80.0, 95.0, 65.0), "f_middle": (80.0, 95.0, 65.0),
            "f_ring": (80.0, 95.0, 65.0), "f_pinky": (80.0, 95.0, 65.0)}
CLEARANCE = 0.002  # metres a finger keeps from the surface it holds
RELAXED = 0.3      # curl of a free finger with no gripping neighbour, or a free thumb
SETTLE = 0.002     # metres of extra room worth moving a finger stuck inside the held mesh


def _rotation(axis, degrees):
    a = np.radians(degrees); c, s = np.cos(a), np.sin(a)
    x, y, z = axis
    return np.array([[c + x * x * (1 - c), x * y * (1 - c) - z * s, x * z * (1 - c) + y * s],
                     [y * x * (1 - c) + z * s, c + y * y * (1 - c), y * z * (1 - c) - x * s],
                     [z * x * (1 - c) - y * s, z * y * (1 - c) + x * s, c + z * z * (1 - c)]])


def _align(a, b):
    """The smallest rotation taking unit direction a to unit direction b."""
    v = np.cross(a, b); c = float(a @ b)
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3) if c > 0 else _rotation(np.cross(a, [1.0, 0, 0] if abs(a[0]) < 0.9 else [0, 1.0, 0]) / np.linalg.norm(np.cross(a, [1.0, 0, 0] if abs(a[0]) < 0.9 else [0, 1.0, 0])), 180)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


def long_axis(points, away_from):
    """A mesh's longest principal axis, pointing away from a point (the holding hand)."""
    _, _, axes = np.linalg.svd(points - points.mean(0))
    return axes[0] if axes[0] @ (points.mean(0) - away_from) >= 0 else -axes[0]


def _about(point, rotation):
    m = np.eye(4); m[:3, :3] = rotation; m[:3, 3] = point - rotation @ point
    return m


def winding(points, triangles):
    """Generalized winding number of a triangle mesh around each point: about
    one inside, zero outside, even for a mesh with a few holes or flipped faces."""
    a, b, c = (triangles[None, :, k] - points[:, None] for k in range(3))
    la, lb, lc = (np.linalg.norm(v, axis=-1) for v in (a, b, c))
    det = (a * np.cross(b, c)).sum(-1)
    div = la * lb * lc + (a * b).sum(-1) * lc + (a * c).sum(-1) * lb + (b * c).sum(-1) * la
    return np.arctan2(det, div).sum(1) / (2 * np.pi)


def signed_distance(points, triangles):
    """Distance from each point to a triangle mesh, negative inside it."""
    a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
    p = points[:, None, :]
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = (ab * ap).sum(-1), (ac * ap).sum(-1)
    bp = p - b; d3, d4 = (ab * bp).sum(-1), (ac * bp).sum(-1)
    cp = p - c; d5, d6 = (ab * cp).sum(-1), (ac * cp).sum(-1)
    va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2
    denom = np.where(np.abs(va + vb + vc) < 1e-18, 1e-18, va + vb + vc)
    v, w = vb / denom, vc / denom
    closest = a + v[..., None] * ab + w[..., None] * ac
    # Clamp to the triangle's edges and corners (Ericson's regions).
    def edge(p0, p1):
        e = p1 - p0; t = np.clip(((p - p0) * e).sum(-1) / ((e * e).sum(-1) + 1e-18), 0, 1)
        return p0 + t[..., None] * e
    inside = (va >= 0) & (vb >= 0) & (vc >= 0)
    candidates = np.stack([edge(a, b), edge(b, c), edge(c, a)])
    distances = np.linalg.norm(candidates - p[None], axis=-1)
    on_edge = candidates[np.argmin(distances, axis=0), np.arange(points.shape[0])[:, None], np.arange(triangles.shape[0])[None, :]]
    closest = np.where(inside[..., None], closest, on_edge)
    gap = np.linalg.norm(p - closest, axis=-1).min(axis=1)
    return np.where(winding(points, triangles) > 0.5, -gap, gap)


class Hand:
    """One hand's finger chains, bend axes and finger vertices, at bind."""

    def __init__(self, character, side, hand_points, hand_bones):
        self.c = character
        index = {n: k for k, n in enumerate(character.names)}
        self.hand = index[f"DEF-hand.{side}"]
        rest = character.rest
        head = lambda b: rest[b][:3, 3]
        self.chains = {f: [index[f"DEF-{f}.0{k}.{side}"] for k in (1, 2, 3)] for f in FINGERS
                       if all(f"DEF-{f}.0{k}.{side}" in index for k in (1, 2, 3))}
        self.palms = [index[n] for n in (f"DEF-palm.0{k}.{side}" for k in range(1, 5)) if n in index]
        # The palm and four fingers at bind (the thumb gets out of the way itself).
        own = [self.hand, *self.palms, *(b for f, bones in self.chains.items() if f != "thumb" for b in bones)]
        self.points = hand_points[np.isin(hand_bones, own)]
        # The palm faces where the relaxed fingers droop: away from the back of the hand.
        middle = self.chains["f_middle"]
        along = head(middle[2]) - head(middle[0]); along /= np.linalg.norm(along)
        sag = head(middle[1]) - (head(middle[0]) + head(middle[2])) / 2
        palm = sag - along * (sag @ along)
        self.palm = palm / np.linalg.norm(palm) if np.linalg.norm(palm) > 1e-5 else np.array([0, -1.0, 0])
        self.axes, self.vertices, self.swings = {}, {}, {}
        for finger, bones in self.chains.items():
            direction = head(bones[1]) - head(bones[0]); direction /= np.linalg.norm(direction)
            towards = self.palm - direction * (self.palm @ direction)
            self.axes[finger] = np.cross(direction, towards / np.linalg.norm(towards))
            self.vertices[finger] = [hand_points[hand_bones == b] for b in bones]

    def curled(self, finger, amount):
        """Bind-space transforms (rest to curled) for a finger's three bones,
        after the finger's swing about its base, if it has one."""
        bones = self.chains[finger]; rest = self.c.rest
        out, chain = [], self.swings.get(finger, np.eye(4))
        for k, b in enumerate(bones):
            chain = chain @ _about(rest[b][:3, 3], _rotation(self.axes[finger], MAX_BEND[finger][k] * amount))
            out.append(chain.copy())
        return out

    def aim_thumb(self, held, placement, toward, down, most=60.0, samples=2000):
        """Swing the whole thumb about its base so it lies along the held mesh,
        pointing toward its far end (`toward`, bind space) and drooping (toward
        `down`) as a relaxed thumb does: a thumb-forward grip. Directions within
        `most` degrees of both the modelled thumb and `toward` are tried; the
        best one whose outer two bones stay clear wins. The base bone may sit
        in a handle the hand was modelled around, so it is not tested. If none
        clears (a thumb modelled wrapped round a handle can't turn forward),
        the thumb keeps its modelled direction."""
        t1, t2, t3 = self.chains["thumb"]; rest = self.c.rest
        base = rest[t1][:3, 3]; own = rest[t3][:3, 3] - base; own /= np.linalg.norm(own)
        toward, down = placement[:3, :3].T @ toward, placement[:3, :3].T @ down  # into the modelled hand's frame
        outer = [v for v in self.vertices["thumb"][1:] if len(v)]
        if not outer: return
        outer = np.c_[np.concatenate(outer), np.ones(sum(len(v) for v in outer))]
        # Evenly spread directions (a Fibonacci sphere), best first.
        k = np.arange(samples) + 0.5
        z = 1 - 2 * k / samples; r = np.sqrt(1 - z * z); a = np.pi * (1 + 5 ** 0.5) * k
        aims = np.c_[r * np.cos(a), r * np.sin(a), z]
        aims = aims[(aims @ own >= np.cos(np.radians(most))) & (aims @ toward >= np.cos(np.radians(most)))]
        for aim in aims[np.argsort(-(aims @ toward + 0.5 * aims @ down))]:
            swing = _about(base, _align(own, aim))
            if signed_distance((outer @ (placement @ swing).T)[:, :3], held).min() >= CLEARANCE:
                self.swings["thumb"] = swing
                return

    def finger_points(self, finger, amount, placement=np.eye(4)):
        moves = self.curled(finger, amount)
        parts = [(placement @ m @ np.c_[v, np.ones(len(v))].T).T[:, :3] for m, v in zip(moves, self.vertices[finger]) if len(v)]
        return np.concatenate(parts) if parts else np.zeros((0, 3))

    def close(self, held, placement=np.eye(4), limit=1.0, steps=60):
        """A curl per finger that hugs the held triangles from outside.

        A finger that starts outside curls until it would touch (the largest
        clear curl). A finger that starts through the held mesh, as a hand
        modelled around a handle does, curls until it has just come out
        round it (the smallest clear curl), and a thumb that can't may open
        instead. If nothing clears, the curl with the most room, unless that
        gains less than SETTLE over the modelled finger (a hand modelled a
        little into its handle stays a little in). A
        finger that touches nothing however far it curls (one off the end of
        a handle) relaxes like its nearest neighbour that does, rather than
        closing into a fist."""
        clear = lambda finger, amount: signed_distance(self.finger_points(finger, amount, placement), held).min()
        def largest_clear(finger, high):
            low = 0.0
            if clear(finger, high) >= CLEARANCE: return high
            for _ in range(14):
                mid = (low + high) / 2
                if clear(finger, mid) >= CLEARANCE: low = mid
                else: high = mid
            return low
        curls, free = {}, []
        for finger in self.chains:
            if clear(finger, 0.0) >= CLEARANCE:
                if clear(finger, limit) >= CLEARANCE: free.append(finger)
                else: curls[finger] = largest_clear(finger, limit)
                continue
            samples = [(clear(finger, limit * k / steps), limit * k / steps) for k in range(steps + 1)]
            cleared = [amount for room, amount in samples if room >= CLEARANCE]
            if not cleared and finger == "thumb":
                cleared = [-limit * k / steps for k in range(1, steps + 1) if clear(finger, -limit * k / steps) >= CLEARANCE][:1]
            room, amount = max(samples)
            curls[finger] = cleared[0] if cleared else amount if room - samples[0][0] > SETTLE else 0.0
        order = [f for f in FINGERS if f in self.chains]
        for finger in free:
            near = sorted((f for f in curls if f != "thumb"), key=lambda f: abs(order.index(f) - order.index(finger)))
            relaxed = curls[near[0]] if near and finger != "thumb" else RELAXED
            curls[finger] = largest_clear(finger, min(relaxed, limit))
        return curls

    def worlds(self, curls, hand_world):
        """Palm and finger bone worlds for a hand placed at hand_world: the
        modelled hand moved there, its fingers curled."""
        placement = hand_world @ np.linalg.inv(self.c.rest[self.hand])
        moves = {b: np.eye(4) for b in self.palms}
        for finger, amount in curls.items():
            moves.update(zip(self.chains[finger], self.curled(finger, amount)))
        out = {}
        for b, move in moves.items():
            world = placement @ move @ self.c.rest[b]
            u, _, vt = np.linalg.svd(world[:3, :3]); world[:3, :3] = u @ vt
            out[b] = world
        return out


def support_placement(hand, held_points, held_triangles, holding_world, along=0.04, gap=0.012, slant=35.0):
    """Bind-space world for a support hand under a held mesh: palm up against
    its underside, `along` metres ahead of its middle toward the far end from
    the holding hand, fingers pointing across it and slanted `slant` degrees
    toward that end, as a hand reaching forward from a dropped elbow lies. The
    hand then settles: it rises until its palm and fingers nearly touch the
    mesh, or drops until they are clear of it, so each finger curls onto it
    from outside.

    "Up" is whichever side of the held mesh faces up while it is held at
    holding_world: the holding hand's walk world times the inverse of its
    bind world, which maps bind space to walk space."""
    c = hand.c
    centre = held_points.mean(0)
    _, _, axes = np.linalg.svd(held_points - centre)
    height, width = axes[1], axes[2]
    holder = c.rest[[k for k, n in enumerate(c.names) if n.startswith("DEF-hand.") and k != hand.hand][0]][:3, 3]
    length = long_axis(held_points, holder)                                # toward the muzzle
    up_now = np.linalg.inv(holding_world[:3, :3])[:, 1]                    # world up, seen in bind space
    up = height if height @ up_now > 0 else -height
    if abs(height @ up_now) < abs(width @ up_now):                        # the thin side faces up
        up, width = (width if width @ up_now > 0 else -width), height
    offsets = (held_points - centre) @ np.c_[length, up, width]
    # The underside right above the palm, not the mesh's lowest point.
    near = np.abs(offsets[:, 0] - along) < 0.03
    point = centre + length * along + up * (offsets[near if near.any() else slice(None), 1].min() - gap)
    # The hand's own frame at rest: wrist to middle knuckle, and its palm.
    rest = c.rest; wrist = rest[hand.hand][:3, 3]
    knuckle = rest[hand.chains["f_middle"][0]][:3, 3]
    fingers = (knuckle - wrist) / np.linalg.norm(knuckle - wrist)
    palm = hand.palm - fingers * (hand.palm @ fingers); palm /= np.linalg.norm(palm)
    # Palm up toward the underside; fingers across, toward the body's other side.
    toward = np.linalg.inv(holding_world[:3, :3])[:, 0] * (1 if c.names[hand.hand].endswith(".R") else -1)
    across = width if width @ toward > 0 else -width
    across = np.cos(np.radians(slant)) * across + np.sin(np.radians(slant)) * length
    frame_rest = np.c_[fingers, palm, np.cross(fingers, palm)]
    frame_new = np.c_[across, up, np.cross(across, up)]
    turn = frame_new @ frame_rest.T
    palm_centre = (wrist + knuckle) / 2 + palm * 0.012
    world = rest[hand.hand].copy()
    world[:3, :3] = turn @ rest[hand.hand][:3, :3]
    world[:3, 3] = point - turn @ (palm_centre - wrist)
    points = np.c_[hand.points, np.ones(len(hand.points))]
    room = lambda w: signed_distance((points @ (w @ np.linalg.inv(rest[hand.hand])).T)[:, :3], held_triangles).min()
    step = 0.002 if room(world) >= CLEARANCE else -0.002
    for _ in range(50):
        moved = world.copy(); moved[:3, 3] += up * step
        if (room(moved) >= CLEARANCE) != (step > 0): return moved if step < 0 else world
        world = moved
    return world


def clear_of(points, triangles, direction, margin=0.005, most=0.10, step=0.0025):
    """How far `points` (a held mesh) must move along `direction` until none
    is inside the `triangles` (the body), plus `margin`; 0 if none is inside,
    None if `most` metres don't suffice."""
    for k in range(int(round(most / step)) + 1):
        if not (winding(points + direction * (k * step), triangles) > 0.5).any():
            return 0.0 if k == 0 else k * step + margin
    return None
