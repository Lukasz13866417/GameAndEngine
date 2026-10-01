"""Re-key a Rigify biped's walk cycle for the engine.

import_model.py passes the clip's bone worlds (engine space: +Y up, facing +Z,
metres) and gets new ones:

- Feet are planted: through the stance each moves back with the ground at
  exactly Gait.speed, with a heel strike, a flat foot and a toe-off roll,
  then swings forward on a smooth arc.
- The pelvis sinks into a slight crouch, bobs twice per cycle (lowest at each
  heel strike), sways over the standing leg, turns with the stride and drops
  the swinging hip a little.
- The chest counter-turns, so the upper body and whatever the hands hold keep
  their aim and only ride the bob.
- Legs and arms are solved with two-bone IK at their modelled lengths, so no
  limb stretches. Each limb bone points exactly at the next joint, and the
  forearm takes the hand's twist instead of the wrist.
- The hands stay where the original placed them, unless grip.py's grips say
  otherwise: then the fingers close on what the hand holds, and a support
  hand rides under it.
- Everything else hangs from its parent at its modelled place, with the
  original's turn, so no joint opens.

Every re-keyed bone is rigid: the rotation and translation the engine's rig
takes, with a scale of one. Needs numpy only.
"""
from dataclasses import dataclass
import numpy as np


@dataclass
class Gait:
    speed: float = 0.95           # m/s the ground moves back under planted feet
    stance: float = 0.6           # fraction of the cycle a foot is down
    crouch: float = 0.03          # m the pelvis sits below its rest height
    bob: float = 0.015            # m, up and down twice per cycle
    sway: float = 0.02            # m, toward the standing leg
    turn: float = 4.0             # degrees the pelvis turns with the stride
    hip_drop: float = 2.0         # degrees the swinging hip drops
    counter: float = 0.85         # share of the pelvis turn the chest undoes
    lean: float = 3.0             # degrees the upper body leans forward
    lift: float = 0.07            # m the ankle rises at mid-swing
    heel_strike: float = 15.0     # degrees the toes point up at heel strike
    toe_off: float = 35.0         # degrees the heel is raised at toe-off
    # Holding something with both hands (see rekey_walk's `hands`):
    blade: float = 12.0           # degrees the shoulders turn to bring the support side forward
    protract: float = 10.0        # degrees the support shoulder rolls forward
    dip: float = 10.0             # degrees the held mesh's far end dips from the authored aim
    lower: float = 0.08           # m the held mesh sits below the authored aim


def _rotation(axis, degrees):
    a = np.radians(degrees); c, s = np.cos(a), np.sin(a)
    x, y, z = axis
    return np.array([[c + x * x * (1 - c), x * y * (1 - c) - z * s, x * z * (1 - c) + y * s],
                     [y * x * (1 - c) + z * s, c + y * y * (1 - c), y * z * (1 - c) - x * s],
                     [z * x * (1 - c) - y * s, z * y * (1 - c) + x * s, c + z * z * (1 - c)]])


def _align(a, b):
    """The smallest rotation taking direction a to direction b."""
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(a @ b)
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3) if c > 0 else _rotation(np.array([1.0, 0, 0]) if abs(a[0]) < 0.9 else np.array([0, 1.0, 0]), 180)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


def _aim(rotation, axis, direction):
    """A bone rotation turned the least so its own `axis` points along `direction`."""
    return _align(rotation @ axis, direction) @ rotation


def _twist(r, axis):
    """Signed degrees rotation r turns about a unit axis, once its swing is undone."""
    side = np.cross(axis, [1.0, 0, 0] if abs(axis[0]) < 0.9 else [0, 1.0, 0]); side /= np.linalg.norm(side)
    turned = _align(r @ axis, axis) @ r @ side
    return np.degrees(np.arctan2(np.cross(side, turned) @ axis, side @ turned))


def _rigid(m):
    """Rotation and translation of a similarity matrix, scale dropped."""
    u, _, vt = np.linalg.svd(m[:3, :3]); out = np.eye(4); out[:3, :3] = u @ vt; out[:3, 3] = m[:3, 3]
    return out


def _pose(rotation, position):
    out = np.eye(4); out[:3, :3] = rotation; out[:3, 3] = position
    return out


def _delta(pivot, offset, rotation):
    """Rotate about a pivot, then move it by an offset."""
    out = np.eye(4); out[:3, :3] = rotation; out[:3, 3] = pivot + offset - rotation @ pivot
    return out


def _two_bone(root, target, upper, lower, pole):
    """Middle joint of a two-bone chain reaching for target, bending toward pole."""
    to = target - root; d = np.linalg.norm(to); direction = to / d
    d = np.clip(d, abs(upper - lower) + 1e-4, upper + lower - 1e-4)
    along = (upper * upper - lower * lower + d * d) / (2 * d)
    height = np.sqrt(max(upper * upper - along * along, 0.0))
    bend = pole - direction * (pole @ direction); bend /= np.linalg.norm(bend)
    return root + direction * along + bend * height, root + direction * d


def _smooth(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * x * (x * (6 * x - 15) + 10)


class WalkRig:
    """Bone indices and rest measurements a walk needs, from a Character."""

    def __init__(self, character, sole_points):
        self.c = character
        self.index = {n: k for k, n in enumerate(character.names)}
        rest = character.rest
        self.head = lambda n, worlds=rest: worlds[self.index[n]][:3, 3]
        # Where a bone's head sits in its parent's frame, as modelled.
        self.offset = lambda b: (np.linalg.inv(rest[character.parents[b]]) @ rest[b][:, 3])[:3]
        self.legs, self.arms = {}, {}
        for side in ("L", "R"):
            leg = [f"DEF-thigh.{side}", f"DEF-thigh.{side}.001", f"DEF-shin.{side}", f"DEF-shin.{side}.001",
                   f"DEF-foot.{side}", f"DEF-toe.{side}"]
            arm = [f"DEF-upper_arm.{side}", f"DEF-upper_arm.{side}.001", f"DEF-forearm.{side}",
                   f"DEF-forearm.{side}.001", f"DEF-hand.{side}"]
            if not all(n in self.index for n in leg + arm):
                raise ValueError("--rekey-walk needs a Rigify biped's DEF leg and arm bones")
            ankle = self.head(leg[4]); ball = self.head(leg[5])
            # The heel strike tilts the foot about the back of its flat sole;
            # the whole boot (sole) then decides how high it must sit.
            sole = sole_points[side]; flat = sole[sole[:, 1] < 0.004]
            heel = flat[np.argmin(flat[:, 2])].copy(); heel[1] = 0.0
            self.legs[side] = dict(bones=leg, ankle=ankle, heel=heel, ball=ball, sole=sole,
                                   upper=np.linalg.norm(self.head(leg[2]) - self.head(leg[0])),
                                   lower=np.linalg.norm(self.head(leg[4]) - self.head(leg[2])),
                                   middle=[np.linalg.norm(self.head(leg[1]) - self.head(leg[0])),
                                           np.linalg.norm(self.head(leg[3]) - self.head(leg[2]))])
            self.arms[side] = dict(bones=arm,
                                   upper=np.linalg.norm(self.head(arm[2]) - self.head(arm[0])),
                                   lower=np.linalg.norm(self.head(arm[4]) - self.head(arm[2])),
                                   middle=[np.linalg.norm(self.head(arm[1]) - self.head(arm[0])),
                                           np.linalg.norm(self.head(arm[3]) - self.head(arm[2]))])
        # Each limb bone's own direction toward the next joint, from its rest pose.
        self.axis = {}
        for part in (*self.legs.values(), *self.arms.values()):
            bones = part["bones"][:5]
            for bone, after in zip(bones, bones[1:]):
                b = self.index[bone]; along = self.head(after) - self.head(bone)
                self.axis[b] = rest[b][:3, :3].T @ (along / np.linalg.norm(along))
        self.hips = self.index["DEF-spine"]
        self.waist = self.index.get("DEF-spine.001")
        self.upper_spine = self.index.get("DEF-spine.002")
        self.neck = self.index.get("DEF-spine.004")
        self.shoulders = {side: self.index[n] for side in ("L", "R") if (n := f"DEF-shoulder.{side}") in self.index}
        # The limbs' IK chains and everything below them (hands, fingers, toes).
        self.limbs = {self.index[n] for part in (*self.legs.values(), *self.arms.values()) for n in part["bones"]}
        for b, parent in enumerate(character.parents):
            if parent in self.limbs: self.limbs.add(b)
        self.pelvis = self.head("DEF-spine")
        self.chest = self.head("DEF-spine.003") if "DEF-spine.003" in self.index else self.pelvis


def _foot(rig, side, phase, gait, period):
    """Ankle position, foot rotation and toe rotation (both absolute) at a cycle phase."""
    leg = rig.legs[side]
    rest_foot = rig.c.rest[rig.index[leg["bones"][4]]][:3, :3]
    rest_toe = rig.c.rest[rig.index[leg["bones"][5]]][:3, :3]
    lateral = np.array([1.0, 0.0, 0.0])
    ankle, heel, ball, sole = leg["ankle"], leg["heel"], leg["ball"], leg["sole"]
    # A planted foot is the rest foot moved back with the ground; at mid-stance
    # its ankle is under the hip.
    hip_z = rig.head(leg["bones"][0])[2]
    def moved(s):
        return np.array([0.0, 0.0, hip_z - ankle[2] + gait.speed * period * (gait.stance / 2 - s)])
    def stance(s):
        flat_from, heel_off = 0.08, 0.40
        if s < flat_from:
            # Heel strike: toes up, rolling onto the heel; the lowest point of
            # the boot sits on the floor.
            turn = _rotation(lateral, -gait.heel_strike * (1 - _smooth(s / flat_from)))
            pivot = heel + moved(s)
            lift = max(0.0, -float(((sole - heel) @ turn.T + pivot)[:, 1].min()))
            return pivot + turn @ (ankle - heel) + np.array([0.0, lift, 0.0]), turn, turn @ rest_toe
        if s < heel_off:
            return ankle + moved(s), np.eye(3), rest_toe
        # Toe-off: the toes stay flat and planted; the foot hinges at the ball joint.
        turn = _rotation(lateral, gait.toe_off * _smooth((s - heel_off) / (gait.stance - heel_off)))
        joint = ball + moved(s)
        return joint + turn @ (ankle - ball), turn, rest_toe
    if phase < gait.stance:
        position, turn, toe = stance(phase)
        return position, turn @ rest_foot, toe
    u = (phase - gait.stance) / (1 - gait.stance)
    start, _, _ = stance(gait.stance - 1e-6)
    end, _, _ = stance(0.0)
    # The foot leaves and meets the ground moving with it (back at `speed`), so
    # a Hermite curve with those end velocities; the lift arcs in between,
    # rising quickly off the toe and settling gently onto the heel.
    ground = np.array([0.0, 0.0, -gait.speed * period * (1 - gait.stance)])
    h00, h10, h01, h11 = 2 * u**3 - 3 * u**2 + 1, u**3 - 2 * u**2 + u, -2 * u**3 + 3 * u**2, u**3 - u**2
    rise = np.sin(np.pi * u) ** 0.75 if u < 0.5 else np.sin(np.pi * u)
    position = h00 * start + h10 * ground + h01 * end + h11 * ground + np.array([0.0, gait.lift * rise, 0.0])
    # Toe-off tilt eases through a slightly raised heel at mid-swing to the heel-strike tilt.
    angle = (gait.toe_off * (1 - _smooth(u / 0.5)) + 5 * _smooth(u / 0.5)) if u < 0.5 else \
            (5 * (1 - _smooth((u - 0.5) / 0.5)) - gait.heel_strike * _smooth((u - 0.5) / 0.5))
    turn = _rotation(lateral, angle)
    toe_bend = _rotation(lateral, -gait.toe_off * (1 - _smooth((u - 0.15) / 0.3)))  # the toe straightens once clear
    return position, turn @ rest_foot, turn @ toe_bend @ rest_toe


def rekey_walk(character, original_worlds, fps, sole_points, gait=Gait(), hands=None):
    """New bone worlds for every frame of a looping walk (same frame count and
    rate), and the largest distance IK fell short of a target, in metres.

    `hands` optionally maps a side ("L", "R") to dict(hand=grip.Hand,
    curls=..., support=bind world or None, carry=bind-space transform). Its
    fingers take that grip; a support hand follows the other hand's held mesh
    to its bind placement; a holding hand moves what it holds by `carry`
    (applied in bind space: a dip, a pull out of the body). With a support
    hand the shoulders turn (`blade`) to bring it forward, while the held
    mesh keeps its aim and the head keeps facing ahead."""
    hands = hands or {}
    support = next((side for side, grip in hands.items() if grip.get("support") is not None), None)
    blade = 0.0 if support is None else gait.blade * (1 if support == "R" else -1)  # + turns the right side forward
    rig = WalkRig(character, sole_points)
    frames = len(original_worlds); period = frames / fps
    up, forward, across_axis = np.array([0, 1.0, 0]), np.array([0, 0, 1.0]), np.array([1.0, 0, 0])
    out, short = [], 0.0
    for f, original in enumerate(original_worlds):
        phase = (f / frames) % 1.0
        # Pelvis, waist and chest move as rigid deltas about their own pivots.
        yaw = -gait.turn * np.cos(2 * np.pi * phase)
        drop = gait.hip_drop * np.sin(2 * np.pi * phase)
        down = -gait.crouch - gait.bob * np.cos(4 * np.pi * phase)
        across = gait.sway * np.sin(2 * np.pi * phase)
        pelvis = _delta(rig.pelvis, np.array([across, down, 0.0]), _rotation(up, yaw) @ _rotation(forward, drop))
        chest = _delta(rig.chest, np.array([across * 0.7, down, 0.0]),
                       _rotation(up, yaw * (1 - gait.counter) + blade) @ _rotation(across_axis, gait.lean))
        waist = _delta((rig.pelvis + rig.chest) / 2, np.array([across * 0.85, down, 0.0]),
                       _rotation(up, yaw * (1 - gait.counter / 2) + blade / 2) @ _rotation(across_axis, gait.lean / 2))
        # Each delta is nudged (by under a centimetre) so the spine stays joined:
        # the waist starts where the pelvis ends, the chest where the waist ends.
        def attach(delta, bone, below):
            moved = delta.copy()
            moved[:3, 3] += (below @ np.r_[rig.offset(bone), 1.0])[:3] - (delta @ _rigid(original[bone]))[:3, 3]
            return moved
        if rig.waist is not None and rig.upper_spine is not None:
            waist = attach(waist, rig.waist, pelvis @ _rigid(original[character.parents[rig.waist]]))
            chest = attach(chest, rig.upper_spine, waist @ _rigid(original[rig.waist]))
        # What the hands hold keeps the authored aim: the chest without the blade.
        aim = _delta((chest @ np.r_[rig.chest, 1.0])[:3], np.zeros(3), _rotation(up, -blade)) @ chest
        worlds = [None] * len(original); worlds[0] = np.eye(4)
        def hang(b):
            """A bone's original turn on its parent, made rigid, at its modelled
            place there (the original stretched some bones)."""
            parent = character.parents[b]
            local = _rigid(np.linalg.inv(original[parent]) @ original[b]); local[:3, 3] = rig.offset(b)
            return worlds[parent] @ local
        def joint(b):
            """Where a bone's head sits on its parent's world."""
            return (worlds[character.parents[b]] @ np.r_[rig.offset(b), 1.0])[:3]
        # The trunk: three spine bones take the deltas, and the rest of the
        # body (shoulders, neck, head, face) hangs from them.
        moved = {rig.hips: pelvis, rig.waist: waist, rig.upper_spine: chest}
        for b in range(1, len(original)):
            if b in rig.limbs: continue
            if b in moved: worlds[b] = moved[b] @ _rigid(original[b])
            elif character.parents[b] == 0: worlds[b] = chest @ _rigid(original[b])  # a loose bone rides the chest
            else: worlds[b] = hang(b)
            # The neck turns the head back to the front; the support shoulder rolls forward.
            turn = -blade if b == rig.neck else blade / abs(blade) * gait.protract if blade and b == rig.shoulders.get(support) else 0.0
            if turn: worlds[b] = _delta(worlds[b][:3, 3], np.zeros(3), _rotation(up, turn)) @ worlds[b]
        # Legs: planted feet, IK at modelled lengths, knees to the front.
        for side, offset in (("L", 0.0), ("R", 0.5)):
            leg = rig.legs[side]; b = [rig.index[n] for n in leg["bones"]]
            target, foot_rotation, toe_rotation = _foot(rig, side, (phase + offset) % 1.0, gait, period)
            hip = joint(b[0])
            knee, ankle = _two_bone(hip, target, leg["upper"], leg["lower"], pelvis[:3, :3] @ forward)
            short = max(short, float(np.linalg.norm(target - ankle)))
            for (first, second), start, end, length in (((b[0], b[1]), hip, knee, leg["middle"][0]),
                                                        ((b[2], b[3]), knee, ankle, leg["middle"][1])):
                direction = (end - start) / np.linalg.norm(end - start)
                worlds[first] = _pose(_aim(_rigid(original[first])[:3, :3], rig.axis[first], direction), start)
                worlds[second] = _pose(_aim(_rigid(original[second])[:3, :3], rig.axis[second], direction),
                                       start + direction * length)
            worlds[b[4]] = _pose(foot_rotation, ankle)
            offset_to_toe = rig.head(leg["bones"][5]) - leg["ankle"]
            worlds[b[5]] = _pose(toe_rotation, ankle + foot_rotation @ np.linalg.inv(character.rest[b[4]][:3, :3]) @ offset_to_toe)
        # Arms: the chest carries the hands; IK puts the elbows back at modelled
        # lengths. A holding hand goes first, so a support hand can follow it.
        for side in sorted(("L", "R"), key=lambda k: hands.get(k, {}).get("support") is not None):
            arm = rig.arms[side]; b = [rig.index[n] for n in arm["bones"]]
            shoulder = joint(b[0])
            hand = aim @ _rigid(original[b[4]])
            if hands.get(side, {}).get("carry") is not None:
                hand = hand @ np.linalg.inv(character.rest[b[4]]) @ hands[side]["carry"] @ character.rest[b[4]]
            outward = across_axis if side == "L" else -across_axis
            # Elbows hang rather than wing out: a holding elbow points out and a
            # little down, a support elbow out and down, under what it holds
            # but clear of the chest.
            pole = outward - up * 0.45
            if hands.get(side, {}).get("support") is not None:
                holder = rig.index["DEF-hand." + ("L" if side == "R" else "R")]
                hand = _rigid(worlds[holder] @ np.linalg.inv(character.rest[holder]) @ hands[side]["support"])
                pole = outward - up
            elbow, wrist = _two_bone(shoulder, hand[:3, 3], arm["upper"], arm["lower"], chest[:3, :3] @ pole)
            short = max(short, float(np.linalg.norm(hand[:3, 3] - wrist)))
            # Each segment points exactly at the next joint, so no joint opens a gap.
            for (first, second), start, end, length in (((b[0], b[1]), shoulder, elbow, arm["middle"][0]),
                                                        ((b[2], b[3]), elbow, wrist, arm["middle"][1])):
                direction = (end - start) / np.linalg.norm(end - start)
                worlds[first] = _pose(_aim(_rigid(chest @ original[first])[:3, :3], rig.axis[first], direction), start)
                worlds[second] = _pose(_aim(_rigid(chest @ original[second])[:3, :3], rig.axis[second], direction),
                                       start + direction * length)
            worlds[b[4]] = _pose(hand[:3, :3], wrist)
            # The authored upper arm rolls about 100 degrees between its two
            # halves, which pinches it in the middle; the shoulder takes half.
            def roll(parent, child, along):
                change = (worlds[parent][:3, :3].T @ worlds[child][:3, :3]) @ \
                         (character.rest[parent][:3, :3].T @ character.rest[child][:3, :3]).T
                return _twist(change, worlds[parent][:3, :3].T @ along)
            upper = (elbow - shoulder) / np.linalg.norm(elbow - shoulder)
            share = (roll(b[0], b[1], upper) - roll(character.parents[b[0]], b[0], upper)) / 2
            worlds[b[0]][:3, :3] = _rotation(upper, share) @ worlds[b[0]][:3, :3]
            # A wrist barely twists; the forearm does. Its lower half turns two
            # thirds of the way with the hand, so the middle of the forearm and
            # the wrist share the twist. The elbow (with its rigid pad) keeps none.
            rest_hand = worlds[b[3]][:3, :3] @ character.rest[b[3]][:3, :3].T @ character.rest[b[4]][:3, :3]
            along = (wrist - elbow) / np.linalg.norm(wrist - elbow)
            twist = _twist(hand[:3, :3] @ rest_hand.T, along)
            worlds[b[3]][:3, :3] = _rotation(along, twist * 2 / 3) @ worlds[b[3]][:3, :3]
        for side, grip in hands.items():
            worlds_by_bone = grip["hand"].worlds(grip["curls"], worlds[rig.index["DEF-hand." + side]])
            for bone, world in worlds_by_bone.items(): worlds[bone] = world
        # Whatever is left (an ungripped hand's palm and fingers) hangs as it did.
        for b in range(1, len(original)):
            if worlds[b] is None: worlds[b] = hang(b)
        out.append(worlds)
    return out, short
