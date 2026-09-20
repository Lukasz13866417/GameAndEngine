# Solar flyby

```sh
cmake -S . -B build
cmake --build build --target vng_solar_flyby_demo -j4
./build/vng_solar_flyby_demo
```

The camera sits closer to the sun, which fills more of the left side. The ship
begins below, slightly left of, and behind the fixed camera,
emerges from the bottom around five seconds, banks right, then recedes around
the sun's right flank. The 24-second shot holds its endpoint; `R` restarts it.
`Space` pauses, `B` toggles bloom, `W` toggles white surface spots (initially off),
and `Esc` closes. This is an authored cinematic curve, not an orbital simulation.

## Scene and rendering

[solar_flyby.vscene](../examples/assets/solar_flyby.vscene) holds the camera,
sun position/radius, cubic Bezier flight handles, duration, background and bloom.
It references the existing original `spaceship.vmesh`.

The executable reuses `examples/spaceflight.cpp`, with this scene as its default;
`vng_spaceflight_demo examples/assets/solar_flyby.vscene` is equivalent. The old
spaceflight scene and standalone sun close-up remain available.

The ship's nose follows the curve tangent, with a smooth bank. Its world-space
light source is the sun's center, using a warm key and restrained cool fill.
Placement, camera and lighting use typed shader arguments. Neither renderer
rebuilds geometry or shaders during animation. The sun's surface and rotating
arcs use the same center and uniform radius; its corona faces the camera.

The render sequence is background, sun, ship into shared linear HDR/depth,
a small linear-HDR softening pass, then bloom/tone mapping into the sRGB display
target. Sun geometry and ship geometry share real depth; the corona and arcs
test depth but do not write it. The light is a cinematic approximation, without
shadows, physical exposure, inverse-square attenuation or lens effects.

## Reproducible inspection

```sh
solar_capture=$(mktemp -d /tmp/vng-solar-flyby.XXXXXX)
./build/vng_solar_flyby_demo --time 7 \
  --screenshot "$solar_capture/closeup.png" --analyze "$solar_capture/evidence"
./build/vng_solar_flyby_demo --time 0 --screenshot "$solar_capture/opening.png"
./build/vng_solar_flyby_demo --time 10 --screenshot "$solar_capture/turn.png"
./build/vng_solar_flyby_demo --time 18 --screenshot "$solar_capture/departure.png"
```

Capture requests default to one frame at the scene's `hero_time` (seven seconds).
Explicit `--time` freezes animation, so pause/restart cannot change that sample.
`--once` without capture flags shows the opening. Use new output paths.

The screenshot is the final composite. `evidence/ship` and `evidence/sun` contain
separate isolated, pre-postprocessing enhanced captures, generated from the same
shader IR and current draw arguments. They include positions, normals, HDR
radiance, surface IDs, depth and generated shaders. They do not represent a
single scene-wide ID buffer or inter-object occlusion capture. The sun's radius
summary measures distance from its placed center, not the world origin.

Tests cover parsed scene validation, tangent-aligned rigid transforms, rightward
screen motion, clearance from the sun, white-spots defaults, placed sun normals
and radiance, empty opening, and shared-depth foreground occlusion in real GL.
