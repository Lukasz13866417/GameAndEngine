# Fleet reveal: beyond the sun

The 34-second shot follows the Kestrel around the sun and opens onto a distant
formation: 17 ships using four mesh blueprints. The carrier, frigate, and escort
are original models generated for this scene; the fourth is the existing Kestrel.
The fleet stays present and visible throughout. Its reveal comes from the sun's
actual depth occlusion, the ships' positions, and the moving camera—not a timed
visibility switch.

## Run the shot

From the repository root:

```sh
cmake --build build --target vng_fleet_reveal_demo vng_editor_demo vng_editor_worker
./build/vng_fleet_reveal_demo
```

Space pauses/resumes, R restarts, and Esc closes. The standalone demo holds its
last pose at 34 seconds instead of looping. `--time` fixes the sampled timestamp;
pause/restart cannot change a fixed sample. `--once` and `--frames N` provide
bounded runs. `--no-bloom` disables bloom without changing the saved scene.

The scene is [fleet_reveal.vscene](../examples/assets/fleet_reveal.vscene), an
ordinary saved editor document. To render an edited copy, pass its filename:

```sh
./build/vng_fleet_reveal_demo path/to/my_fleet.vscene
```

## Tune it in the editor

```sh
./build/vng_editor_demo --scene examples/assets/fleet_reveal.vscene
```

Select the **Tracking camera** instance and click **Inspect**, then **Play (in
editor)** to follow the authored shot. Without a camera visit, playback still
animates the scene but leaves your private inspection camera alone. **Play
(independent)** uses the worker's native window and renders through that same
scene camera. Native camera navigation temporarily overrides the shot; restart
independent Play to resume following it.

Pause and choose a timestamp before authoring changes:

- **Instances:** select a ship in the scene or instance list. Its position,
  rotation, scale, and appearance belong to that instance. Existing animated
  properties get a key at the playhead; unanimated properties edit the instance's
  base values. Applying a ship's transform does not change its blueprint or
  other ships using that blueprint.
- **Camera:** the **Tracking camera** is a scene camera instance. Its position,
  rotation (the look direction), focus distance and zoom are ordinary editable
  tracks. **Enter** it, navigate, then **Save this camera** to key the editor view
  at the selected keyframe; **Inspect** follows the shot read-only.
- **Timing:** select a marker or named keyframe to edit its timestamp, name,
  values, and incoming interpolation. **Blend** means interpolate from the
  previous key to this key. Turn it off for a held value that jumps at the key;
  doing this for the camera properties creates a cut. Existing cuts survive
  camera-control edits at that timestamp.
- **Blueprint geometry:** choose its named **Mesh:** entry in the View dropdown.
  Vertex edits affect the shared model used by every instance of that blueprint.
  Authored colors, normals, and emission remain in the mesh. Normals are not
  recalculated when vertices are deformed.

The initial curved ship and camera paths are baked into one-second linear
samples. They are not hidden C++ animation callbacks: the editor can change,
remove, or add any key. Named milestones make it easier to find the approach,
turn, reveal, and rendezvous among those regular samples. The fleet's small
formation drift is keyed per instance as well.

**Save / Ctrl+S** updates the file you opened. Use **Save As / Ctrl+Shift+S** first
if you want to preserve the supplied shot. The file embeds each blueprint's mesh
once and stores instances and tracks separately; it does not depend on loading
the original `.vmesh` files again. The standalone demo and editor use the same
[Runtime](../examples/editor/runtime.cpp), mesh shaders, sun renderer, starfield,
and postprocessing. There is no separate reduced-quality editor effect.

Global stars/exposure/bloom are stored in the scene's `environment` section;
these knobs currently use file editing. See [editor controls](editor.md) for
details, safe saving, navigation, and independent Play. The editor's normal
playback clock loops at the timeline duration, unlike the standalone demo's
held ending.

## Inspect reproducible output

Choose new output paths; capture does not replace existing files/directories:

```sh
./build/vng_fleet_reveal_demo --time 26 \
  --screenshot /tmp/fleet-reveal-new.png \
  --analyze /tmp/fleet-reveal-new-evidence
```

The screenshot and `composite.png` show the final scene with sun occlusion,
stars, lighting, and bloom. The evidence directory also contains enhanced-shader
source-face views for the pathfinder, flagship, and sun, plus `summary.txt` with
the sampled camera and instance information.

The `*-faces.png` images isolate the selected object. They are **not scene-wide
visibility masks** and cannot prove that another object is hidden by the sun.
Use the composite screenshot for that relationship. The editor's **Face IDs**
toggle has the same isolated-object scope.

## Offline authoring and implementation

[fleet_reveal.cpp](../examples/fleet_reveal.cpp) selects the saved document and
diagnostic subjects; [scene_demo.cpp](../examples/support/scene_demo.cpp) shares
playback, camera sampling and presentation with the [asteroid-belt variant](asteroid_fleet.md).
The initial choreography lives
in [fleet_scene.cpp](../examples/support/fleet_scene.cpp); the fleet model
generator is [make_fleet.cpp](../examples/tools/make_fleet.cpp). Neither generator
runs during normal playback or while opening the scene in the editor.

To generate a fresh scene from the source assets:

```sh
cmake --build build --target vng_make_fleet_scene
./build/vng_make_fleet_scene examples/assets /tmp/fleet-fresh.vscene
```

The destination must not already exist. The optional final `--replace` argument
explicitly replaces it with the generated scene and **discards all editor tweaks
in that file**. Regeneration is not required after ordinary scene editing; Save
and run the edited `.vscene` directly.
