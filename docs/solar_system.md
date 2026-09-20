# Leaving home: Earth, the sun, the belt and the fleet

One editable establishing shot that combines the existing actors: the
stylized Earth blueprint, a sun instance, the Kestrel, the 40-ship fleet and
600 tumbling rocks. Only the starting keyframe exists. The rest of the shot is
meant to be authored in the editor, so nothing here is choreographed.

## Starting frame

- The camera sits just above Earth's surface (about 1.8 Earth radii from its
  centre) and looks toward outer space. The sun is behind the camera's right
  shoulder, out of frame, and lights Earth's visible side and the fleet from
  the front.
- Earth fills the left side of the frame: its centre lies just past the frame
  edge while the near limb is well inside it.
- The Kestrel is a few units behind the camera, low on its right, already
  heading along the view direction.
- The belt is far but plainly visible, spread across the view about 300 units
  out. The fleet waits on the same axis roughly 30% of that distance in front
  of the rocks, so every ship is nearer than every rock.
- The camera's orbit pivot is the fleet. The editor's far plane is four times
  the pivot distance, so the whole belt stays in view when orbiting.

## Run and edit

```sh
cmake --build build --target vng_solar_system_demo vng_editor_demo vng_editor_worker
./build/vng_solar_system_demo
./build/vng_editor_demo --scene examples/assets/solar_system.vscene
```

In the demo, Space pauses, R restarts, and Esc closes. With a single keyframe
the shot holds its starting pose; add camera and instance keys in the editor
to build the departure.

```sh
./build/vng_solar_system_demo --time 0 --screenshot /tmp/leaving-home-new.png
```

Use a new output path; the demo does not overwrite screenshots.

## Source and regeneration

[solar_system.cpp](../examples/solar_system.cpp) configures the shared
[scene playback helper](../examples/support/scene_demo.cpp). The layout lives
in [solar_system_scene.cpp](../examples/support/solar_system_scene.cpp): it
reuses the fleet scene's ship blueprints and named formation, imports
`assets/earth.vmesh`, generates the three rock blueprints, and scatters the belt
with safe separation from every ship. The constants a test can depend on
(distances, identities, Earth radius) are in
[solar_system_scene.hpp](../examples/support/solar_system_scene.hpp).

`vng_solar_system_scene_tests` checks the brief on the CPU: one keyframe,
every actor present, the camera beside Earth looking away from the sun with
Earth at the side, the Kestrel behind the camera, the belt far but centred,
the fleet about 30% of the belt distance in front of it, and a save/load
round trip.

The shipped `examples/assets/solar_system.vscene` is also an editable project.
Tests author their own fixture from the generator rather than reading it, so
editing and saving the shipped scene never breaks them. To regenerate the
shipped file from scratch:

```sh
cmake --build build --target vng_make_solar_system_scene
./build/vng_make_solar_system_scene examples/assets examples/assets/solar_system.vscene --replace
```

Without `--replace` the tool refuses to overwrite an existing scene.
