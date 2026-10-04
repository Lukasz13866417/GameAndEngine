# Characters from Blender

A rigged, animated character made in Blender reaches the engine in three steps:

1. Blender exports it as a binary FBX.
2. `examples/tools/import_model.py` converts it into engine files: a skinned
   `.vmesh` and a `.vrig` armature with its clips.
3. The `vng_character` library loads those files into [`vng_rig`](rigging.md),
   whose skinned renderer deforms the mesh on the GPU.

The soldier in `examples/assets/soldier/` is the first character imported this
way:

```sh
cmake --build build --target vng_soldier_demo
./build/vng_soldier_demo
```

He walks on a floor that scrolls under him at the walk's own speed (0.95 m/s),
so his planted foot stays put. He and the floor turn slowly in front of a fixed
camera. The skinned renderer's one light is fixed in the world, so turning him,
rather than orbiting the camera, keeps him lit from the key side. Beyond 10 m
the floor fades into the sky, which leaves no far edge.

| Option | Effect |
| --- | --- |
| `--in-place` | Keep the floor still. |
| `--still` | Stop the turntable. |
| `--angle DEG` | Camera direction around him: 0 = in front (default 65). |
| `--distance M` | Camera distance (default 3.7). |
| `--look Y` | Height the camera aims at (default 0.85). |
| `--speed X` | Change the playback speed. |
| `--time S` | Start S seconds into the clip. |
| `--clip NAME` | Choose the clip: `walk` (re-keyed) or `walk_authored`. |
| `--mesh PATH` | Show another imported character. |

Keys: Space pauses, Left and Right step one frame, Escape quits.

Two options render in a hidden window and exit. Both return 77 when no OpenGL
window can be made.

- `--screenshot NEW.png` renders one frame at `--time` and saves it.
- `--record NEW_DIR` writes `--seconds` of frames at `--fps` (default 4 s at
  60 fps).

## Importing a model

```sh
python3 examples/tools/import_model.py Soldier.fbx examples/assets/soldier --name soldier \
    --colors examples/assets/soldier/colors.json --lights examples/assets/soldier/lights.json \
    --rekey-walk --hole-closer 0.06 --flatten-deltoids
```

`--hole-closer M` edits the model: a mesh held wholly by one hand that has
two holes through it (the soldier's rifle: a rear and a front hand-hole) gets
its far hole moved M metres nearer the near one. The stretch between them is
shortened; the near hole and what lies behind it, and the far hole and what
lies beyond it, keep their shape. The soldier's rifle is 6 cm shorter, so his
right hand reaches its front hole with the rifle at the authored aim height.

`--flatten-deltoids` edits the model too. The soldier's sleeves bulge on top,
8 to 11 cm out from each shoulder joint, about 2 cm above a straight taper.
With his arms held forward and down, that bulge stood out of the arm's top
line as a step behind the shoulder. In every direction around each upper arm,
the sleeve may now come no further from the bone than the straight line from
its distance 2 cm along the arm to its distance at 20 cm. For the soldier, 61
sleeve vertices move in, by at most 2.9 cm, and the shoulder yoke lying on
them moves with them.

`--lights FILE` adds lights the character carries, from a JSON list. Each
light has a `name`, a `position` and `direction` in the character's bind pose
(engine coordinates, metres; it rides on the skin there), a linear `color`
times its strength,
`inner` and `outer` cone edges in degrees and a `range` in metres. They are
written to the `.vrig` (see [The .vrig format](#the-vrig-format)). With
`lens` (metres), the faces within that distance of the light that face along
it glow in twice its colour. The soldier's `lights.json` gives him his
shoulder flashlight: a warm beam angled 15 degrees down from where it points
as modelled, so it lights the floor ahead of him.

The tool needs Python 3 and numpy. It writes three files into the output
directory:

| File | Contents |
| --- | --- |
| `NAME.vmesh` | The bind-pose mesh, with normals, colours and skin weights. |
| `NAME.vrig` | The armature and every clip. |
| `NAME_pose.vmesh` | The mesh deformed at one clip frame (`--pose-frame N`, default 0), with no skin. It is for static scenes and the editor's mesh import. |

For the soldier, it reads 9 meshes, 11 materials, a 278-bone Rigify rig and six
identical copies of one walk. It writes 17,265 vertices, 9,601 triangles, 109
bones (98 from the rig, a root and 10 joint helpers) and two 24-frame clips at
24 fps, one-second loops:

- `walk`, re-keyed at 0.95 m/s (see [re-keying a walk](#re-keying-a-walk));
- `walk_authored`, the walk as authored in Blender, for comparison. Its fingers
  crumple and its right wrist opens by up to 9 cm, because the engine's bones
  cannot stretch as the authored ones did.

What the conversion does:

- **Space.** +Y is up, the character faces +Z, one unit is one metre, and the
  feet stand at Y = 0. `--height` sets the height in metres (default 1.8); the
  soldier was 5.6 Blender units tall.
- **Bones.** Only bones with skin weights are kept, plus a `root` bone. The
  soldier has 98: 92 `DEF-` bones and 6 of Rigify's face controls, which his
  weights also use.
  - Each bone's parent is its nearest deforming ancestor.
  - Rigify hangs many `DEF-` bones (the shoulders, the breasts, the face) from
    mechanism bones. An `ORG-` ancestor therefore stands for the `DEF-` bone
    that copies it. So the face hangs from the head, and the arms hang from the
    shoulders.
  - Only the hips hang from `root`.
- **Joint helpers.** Each elbow, knee and shoulder gets a `HALF-` bone that
  moves half as far as the joint's two bones do, about the joint. The elbows
  also get `QUARTER-` and `THREEQUARTER-` bones, a quarter and three quarters
  of the way: they fold furthest, the soldier's holding elbow by 128 degrees.
  Linear blend skinning averages the two bones' matrices across a joint, so
  a bent elbow shrinks toward its centre: the soldier's sleeves lost up to
  half their thickness at the elbow, in Blender as well. A vertex shared by
  the two bones should turn part of the way through the joint, by its share
  of the lower bone; its weight goes to the two helpers (or bones) either
  side of that share instead. It turns rigidly with them, so no blend spans
  more than a quarter of the elbow's fold; the elbow pads turn as rigid caps.
- **Elbow weights.** The sleeves handed over from upper arm to forearm
  unevenly, from 9 cm above the elbow to 12 cm below it. The weight the two
  arm bones share is re-split by a smoothstep centred on the joint, so the
  arm bends at the elbow. It spans 12 cm either side on the inside of the
  fold (the crease, toward the front of a T-posed arm) but 5 cm on the
  outside: the back of a forearm stays with the forearm up to the point of
  the elbow, so a folded arm keeps its thickness to the joint instead of
  pinching there. A rigid piece such as an elbow pad stays rigid and takes
  the share the sleeve has under its middle.
- **Underarm weights.** Near the armpit, the back and underside of each
  upper arm (the tricep, opposite the elbow's fold) were shared with the
  torso 10 cm out along the arm. With the arm raised to the rifle that skin
  lagged toward the body, and the arm's underside sagged into a hollow. There,
  fading out toward the sides, the arm's share rises to a smoothstep from 2
  to 10 cm out from the shoulder joint, so the tricep keeps its shape; the
  armpit itself stays blended, so it does not tear away from the back.
- **Shoulder weights.** Where the chest meets the upper arm, the soldier's
  weights jumped between neighbouring vertices, and with the arm raised the
  jumps folded the skin into bumps. Within 15 cm of each shoulder joint, every
  vertex's weights are blended toward its neighbours' four times: by half at
  the joint, fading to nothing at 15 cm. Meshes bound to a single bone keep
  their weights.
- **Doubled triangles.** A mesh can hold the same triangle twice, facing
  opposite ways. Drawn together, the two copies fight over the same pixels
  and the surface shows stripes of light and dark. The soldier's shoulder
  flashlight was built twice over (134 such pairs). Of each pair the copy
  facing away from the middle of its doubled region is kept.
- **Face weights.** Blender's automatic weights let things near the face
  follow the face's bones: the flashlight followed his jaw by about half, so
  it bent when his head turned against his shoulders. Only the mesh with the
  most head-bone weight (his hood) keeps face-bone weights; on others they go
  to each vertex's strongest other bone (198 vertices for the soldier).
- **Unweighted vertices.** Blender leaves unweighted vertices in place. Here
  they take their nearest weighted neighbour's weights, so they move with what
  they touch. The soldier's front belt pouch had 74 such vertices.
- **Poses.** Every clip frame is evaluated the way the FBX file composes
  transforms. Then each bone is fitted to the transforms the rig supports:
  rotation, translation and a positive uniform scale.
  - Rigify's bone stretching comes out of FBX as non-uniform scale and shear.
    These are dropped, and that keeps the rifle rigid.
  - Where they matter, the fitted walk moves points by up to 4% of the
    character's height compared with composing the FBX literally.
  - Blender's segmented, bendy bones never reach FBX, so no importer sees
    them.
- **Skin weights.** Each vertex keeps its eight largest influences, normalized.
  Eight keep every vertex within 0.2% of the character's height of using all of
  them; four would allow 0.9%.
- **Vertices.** A vertex is split wherever its normal or colour differs, as the
  mesh format's notes on importers ask.
- **Walking speed.** The clip's `speed` is what the demo scrolls the floor by.
  - For an imported clip, it is the median speed of the vertex each foot
    stands on: that frame's lowest foot vertex, followed to the next frame,
    while the sole is within 5 cm of its lowest point.
  - For a re-keyed walk, it is exact by construction.

The tool prints what it could not keep. For the soldier:

- 146 vertices had up to 14 influences and keep their 8 largest;
- 48 bones stretch or shear in the FBX;
- as authored, the fitted rig differs from the FBX composed literally by at
  most 7.7 cm (at the rifle's muzzle).

### Re-keying a walk

`--rekey-walk` replaces a clip named `walk` with one made by
`examples/tools/walk_cycle.py`, and keeps the original as `walk_authored`. The
authored walk kept everything above the hips still: only its 12 leg bones were
keyed. Its planted feet also hovered, skated and sank.

The re-keyed walk:

- **Feet** move back with the ground at exactly the clip's speed while they
  are down. Each strikes with its heel, the boot's lowest point resting on the
  floor, goes flat, then hinges at the ball joint while the toes stay planted.
  It then swings forward on a Hermite curve that leaves and meets the ground at
  the ground's speed.
- **Pelvis:** it sits 3 cm low and bobs twice per cycle, lowest at
  each heel strike. It sways over the standing leg, turns with the stride and
  drops the swinging hip a little.
- **Chest:** it undoes most of the pelvis's turn and leans slightly forward,
  so the rifle keeps its aim and only rides the bob.
- **Legs and arms** are solved by two-bone IK at their modelled lengths.
  - The left hand keeps the original's grip on the rifle and its aim at face
    height. `--gait lower=0.08,dip=10` carries it 8 cm lower instead, muzzle
    dipped 10 degrees about the butt.
  - The shoulders turn 12 degrees to bring the right side forward, and the
    right shoulder rolls forward 10 degrees; the rifle keeps its aim and the
    neck turns the head back to the front. That gives the right arm the reach
    to the rifle's front hole with a relaxed elbow.
  - The collarbones follow the arms: each turns by a quarter of its arm's
    swing away from the T-pose, as a shoulder blade does (in people it takes
    about a third). The soldier's turn 22 degrees on the right and 27 on the
    left, which brings each shoulder joint about 8 cm forward. Before, the
    whole swing happened at the shoulder joint and folded the skin around it.
    With the shoulder weights, folds that sharpen by more than 30 degrees
    near the right shoulder fell from 18 edges to 7, and on the left from 12
    to 2.
  - The elbows hang rather than wing out. The holding elbow points out and a
    little down; the support elbow points out and down, clear of the chest.
  - If what the hands hold still sinks into the torso or head, it is pulled
    along its length until it clears (measured by skinning both meshes). The
    authored aim pressed the butt 4.2 cm into his chin, so the rifle sits
    4.2 cm further forward; the lower carry needs no pull.
  - That removes the authored right arm's 1.53x stretch, which the uniform-scale
    fit had turned into a hand 16% too large and a wrist torn 9 cm open.
  - Each limb bone points exactly at the next joint.
  - The arms are a twist-free chain: the upper arm swings from where the
    collarbone carries it, the forearm from where the upper arm carries it,
    each by the least turn. Both halves of each part share one turn, so
    neither rolls in its middle. The authored rolls are not used: they turned
    each upper arm's two halves about 100 degrees apart, and on the low-poly
    sleeves a roll between two rings folds them into a point.
  - The hand's own twist is shared by the shoulder, elbow and wrist, a third
    each (about 30 degrees for the soldier).
- **Everything else** (the neck, head, face and shoulders) hangs from the spine
  with the original's turns. Every re-keyed bone has a scale of 1 and sits at
  its modelled place on its parent, so no joint opens.
- **Hands** take grips from `examples/tools/grip.py`, because the FBX's finger
  matrices are up to 280% non-rigid and the fitted fingers crumple.
  - A mesh bound wholly to one hand is held by that hand. The soldier's rifle
    is bound to his left hand.
  - The holding hand's fingers curl about their knuckles until they touch the
    mesh.
  - The other hand takes the mesh's far hole the way the holding hand takes
    the near one. `grip.holes` finds openings through the mesh across its
    width (lines through its side profile that miss every triangle but lie
    inside its outline); the soldier's rifle has two of about 58 cm2, the
    rear hand-hole and the front hex hole. The holding hand's grip is mirrored
    across the rifle and moved from hole to hole, which the symmetric skeleton
    makes exact, and the fingers curl into place.
  - A mesh with fewer than two holes is supported from below instead: the
    hand goes palm up under it, 4 cm ahead of its middle, fingers slanted 35
    degrees toward the far end, settled against the underside.
  - A thumb swings at its base to lie forward along the mesh if that is within
    60 degrees of how it was modelled; otherwise (both of the soldier's) it
    keeps its modelled direction.
  - A finger that touches nothing relaxes like its neighbour, rather than
    closing into a fist.
  - Grips are solved once, in bind space, so they hold in every frame. The left
    hand was modelled about 1 cm into the rifle's grip, so a few finger vertices
    stay up to 1 cm inside it.

`--gait` tunes it, for example `--gait crouch=0.02,bob=0.02`. The settings are
the fields of `walk_cycle.Gait`: `speed`, `stance`, `crouch`, `bob`, `sway`,
`turn`, `hip_drop`, `counter`, `lean`, `lift`, `heel_strike`, `toe_off`, and
for a two-handed hold `blade`, `protract`, `dip`, `lower` and `reach` (metres
the held mesh sits further forward along its length), and `shoulder_follow`
for the collarbones.
With the defaults, the soldier's lowest point stays within 1 cm of the floor in
every frame, and his hips rise and fall by 3 cm.

### Colours

FBX keeps a material's colour only when it is a plain value. The soldier's
materials take their colour from image textures, so the FBX carries none: its
UVs map each face to a colour, but the images were not exported. Until those
textures are imported, `examples/assets/soldier/colors.json` gives each mesh a
provisional colour, in linear RGB and keyed by mesh or material name. It is a
stand-in, not the model's own look.

## The skinned .vmesh

The mesh is an ordinary `vmesh 1.0` document. Its `info` block names its rig
with `skin/rig = "NAME.vrig";`, relative to the mesh. Influences come in groups
of four extra vertex fields, which ordinary mesh loaders ignore:

```text
skin/bones/0 : u32x4;     # bone indices, in the .vrig's bone order
skin/weights/0 : f32x4;   # their weights; zero for unused slots
skin/bones/1 : u32x4;     # four more
skin/weights/1 : f32x4;
```

Each vertex's weights sum to one. `NAME_pose.vmesh` has no skin fields and
loads like any other mesh.

## The .vrig format

A `.vrig` file is a [structured document](documents.md):

```text
vrig 1.0
rig = 1;
name = "soldier";
source = "Soldier.fbx";
bones = [
    { name = "root"; parent = ""; translation = [0, 0, 0]; rotation = [0, 0, 0, 1]; scale = 1; },
    { name = "DEF-spine"; parent = "root"; translation = [0, 0.914, -0.085]; rotation = [0.091, -0.008, 0.707, 0.701]; scale = 1; },
];
clips = [
    {
        name = "walk";
        fps = 24;
        frames = 24;
        loop = true;
        speed = 0.73;
        tracks = [
            { bone = "DEF-spine"; translation = [...]; rotation = [...]; scale = [...]; },
        ];
    },
];
lights = [
    { name = "flashlight"; position = [-0.147, 1.55, 0.004]; direction = [-0.021, -0.261, 0.965];
      color = [3, 2.7, 2.1]; inner = 9; outer = 20; range = 12; },
];
```

- **Bones.** Bones are listed parents first. Transforms are parent-relative:
  a translation, a unit quaternion `[x, y, z, w]` and a positive uniform
  scale. A bone with an empty `parent` is a root.
- **Tracks.** A track holds one bone's local transform at every frame, flat:
  3 translation values, 4 rotation values and 1 scale value per frame.
- **Looping.** A looping clip lasts `frames / fps` seconds, and its last frame
  blends into its first. A clip that does not loop lasts `(frames - 1) / fps`
  seconds and holds its ends.
- **Lights** (optional). `position` and `direction` are where a light is and
  where it points in the bind pose, in the mesh's space. It rides on the skin
  there: it moves as the mesh vertex nearest it does, so it stays on (and aims
  along) whatever carries it. `color` is linear and times its strength; `inner` and `outer`
  are the edges of its cone in degrees off its axis; it fades out at `range`
  metres. `Character::spots(pose)` gives them as `render::SpotLight`s for a
  pose.

`read_rig` refuses:

- unknown document kinds or versions;
- duplicate or empty names;
- a parent listed after its child;
- rotations that are not unit length to within f32 rounding;
- scales that are zero or negative;
- a non-positive `fps`, or zero frames;
- tracks of the wrong length, or for unknown bones;
- lights with no position, a zero direction, non-finite values, cone
  edges outside 0 <= inner <= outer < 90 degrees or a range that is not
  positive.

## Code

- **`examples/character/character_file.{hpp,cpp}`** parses and validates
  `.vrig` documents. It needs only `vng_content` and `vng_rig`'s value types.
- **`examples/character/character.{hpp,cpp}`** provides `Character::load`,
  which builds the armature and skin binding from the mesh and its rig.
  `pose(clip, seconds, pose)` sets each animated bone, blending neighbouring
  frames: linear translation and scale, and shortest-path rotation.
- **`examples/character/stage.{hpp,cpp}`** is the floor a character walks
  on when shown alone, bound to a single bone so the same skinned renderer
  draws it.
- **`examples/soldier.cpp`** is the demo. His flashlight (`spots`) lights him
  and the floor, turning with him on the turntable.
- **`tests/examples/character_tests.cpp`** (`vng_character_tests`) covers:
  - the format and its refusals;
  - frame blending;
  - the soldier's size and rest pose;
  - his walk:
    - feet within 5 cm of the floor in every frame;
    - a rifle that keeps its shape;
    - a seamless loop;
    - for the re-keyed walk, a foot on the floor every frame, a bobbing pelvis
      and no stretched bone;
    - every joint at its modelled length in every frame, and a right hand that
      stays put on the rifle.

## Limits

- **FBX support.**
  - Only binary FBX 7.x as Blender writes it: triangulated meshes, skin
    clusters and baked curves.
  - Every node must inherit transforms the default way, and no pivots are
    read.
- **Bones.** Uniform scale only; stretch and shear are fitted away.
- **Skinning.** At most eight influences per vertex.
- **Materials.** No textures and no materials: colour is per vertex.
- **Scenes and the editor.** The editor does not load skinned characters; use
  `NAME_pose.vmesh` for a static pose there. Characters are not part of
  `.vscene` scenes. To look at one from every side in each clip and pin
  notes on him, open his `.vmesh` in `vng_review` ([review.md](review.md)).
- **Movement.** There is no root motion beyond the clip's walking speed.
- **Re-keying.** It knows walk cycles of Rigify bipeds only: it needs the DEF
  leg and arm chains.
