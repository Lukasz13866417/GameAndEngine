# Shattered Reach: asteroid-belt fleet reveal

A second, separately saved fleet shot. A close camera follows the Kestrel
through a deep belt, then slows to observe the formation beyond it.
Gaps between separated rocks offer partial glimpses of the distant ships;
occlusion accumulates through the belt's depth, not a solid screen of rocks.
No visibility keys spawn the fleet, and its geometry is inside the camera's
far plane from the start. The 52-second continuous tracking shot eases the
ship and camera to a stop at 44 seconds, then holds for eight seconds.
The ship follows broad turns with a
subtle nine-degree bank, never making a close approach to the fleet. There is
more than 60 units of open space between the belt and the nearest fleet hull;
the pathfinder stays more than 65 units from fleet hulls throughout the shot.
World bounds span `[-180,-120,-420]` to `[180,120,180]` and remain editable
with the editor's world-bounds gizmo.

The scene contains the pathfinder, 40 fleet ships and 600 tumbling rocks. The
scatter volume spans 180 units wide, 90 high and 154 deep, with a denser core
and sparse outskirts. Maximum rock scale is .92. Conservative bounding spheres
remain separated even during rotation; there are no overlapping occluder slabs.
The fleet has capital carriers, frigates, escorts and scouts,
with ship scales ranging from .13 to 2.5 across a deeper formation.
Three original rock
blueprints have displaced silhouettes, crater bowls, layered colors and
normals. Seven mesh blueprints serve all 641 instances; geometry is uploaded
once per blueprint, not once per rock or per animation frame. This is a
deliberately dense cinematic belt, not a scale model of the Solar System.

## Run and edit

```sh
cmake --build build --target vng_asteroid_fleet_demo vng_editor_demo vng_editor_worker
./build/vng_asteroid_fleet_demo
./build/vng_editor_demo --scene examples/assets/asteroid_fleet.vscene
```

In the demo, Space pauses, R restarts, and Esc closes. Run without `--once` to
watch the approach and reveal. In the editor, select the **Tracking camera**,
click **Inspect** and **Play (in editor)** to follow the authored shot. The
independent Play window uses the same renderer and renders through that camera.
The shipped `.vscene` predates scene cameras, so its shot loads as an
**Animation camera** instance that orbits its focus point, as the old camera
did, and keeps the same keys and path; a freshly generated scene names it
**Tracking camera**.

Ship/camera paths and rock rotation are ordinary timeline keys. Named
milestones mark entry, threading the gap, breaking through, and holding position.
Positions/scales belong to instances; editing a rock in the View dropdown edits
its shared blueprint. Save / Ctrl+S updates this scene, without touching the
original sun-based `fleet_reveal.vscene`. Use Save As to preserve this version.

You can rearrange the belt or fleet, edit keys, replace blueprints, or add a
camera cut using the keyframe panel's incoming interpolation switch. The
generated shot uses one-second camera/ship samples and two rotation keys per
rock. Rotation doesn't require re-uploading vertex data. The initial flight
corridor has conservative clearance checks; those are offline tests, not a
runtime collision solver that will constrain your edits.

All geometry is embedded in the scene. The editor can export individual rock
blueprints to `.vmesh`; loading the scene never reruns the procedural generator.
As with other mesh editing, deforming vertices currently retains authored normals
rather than recalculating them.

## Inspect

```sh
./build/vng_asteroid_fleet_demo --time 38 \
  --screenshot /tmp/belt-reveal-new.png --analyze /tmp/belt-evidence-new
./build/vng_asteroid_fleet_demo --time 10 --screenshot /tmp/inside-belt-new.png
```

Use new output paths. Capture defaults to 38 seconds, after leaving the rocks;
the ship finishes braking at 44 seconds and remains well short of the fleet.
Evidence includes the composite, isolated face-ID images for the lead ship,
flagship and exit rock, and sampled instance/camera information. Isolated
face-ID views are not full-scene occlusion masks. The render test compares
the actual composite with/without the fleet and with/without rocks to measure
partial early occlusion and the later unobstructed reveal.

## Source and regeneration

[asteroid_fleet.cpp](../examples/asteroid_fleet.cpp) configures the small shared
[scene playback helper](../examples/scenes/scene_demo.cpp). Both fleet demos
and the editor use the existing editor Runtime; no asteroid-specific renderer,
new engine API, or scene format was needed.

For full-scene keyframe insertion in this larger scene, set **Settings > Timeline
track limit** to **8192**. Existing playback and editing of existing tracks
do not require raising that limit. **Settings > Instance limit** defaults to
4096 (range 1–65536); it controls additions such as import, instantiate and paste,
not loading existing scenes or undo/redo. Both preferences persist separately
from scene content and apply without restarting.

Offline geometry lives in [asteroid_assets.cpp](../examples/support/asteroid_assets.cpp)
and choreography/scattering in [asteroid_scene.cpp](../examples/scenes/asteroid_scene.cpp).
The latter reuses the fleet's existing ship blueprints, not the solar shot's motion.

```sh
cmake --build build --target vng_make_asteroid_scene
./build/vng_make_asteroid_scene examples/assets /tmp/asteroid-fresh.vscene
```

The destination must not exist. A final `--replace` explicitly discards edits
to that generated scene. Ordinary editor changes need only Save; pass the
edited filename to `vng_asteroid_fleet_demo` to play it.
