# Procedural sun demo

Build and run from the repository root:

```sh
cmake -S . -B build
cmake --build build --target vng_sun_demo -j
./build/vng_sun_demo
```

The window stays open and animates. `--once` deliberately exits after one frame.
OpenGL 4.6 core and the GLFW window backend are required, as with the other
graphical examples.

`Space` pauses/resumes animation, `B` toggles bloom, `D` toggles displacement,
`W` toggles the white-hot surface spots (off by default),
`R` resets animation time, and `Esc` closes the window. An explicit `--time`
keeps the sample fixed: pause/reset do not override it, though visual toggles
still work. Start without `--time` for animated playback.

This is a cinematic, SDO-inspired **false-color solar visualization**, not a
photometrically accurate visible-light view or a physical plasma simulation.
The current visual target is the supplied fiery-orange solar reference: dense
turbulent detail over the whole disk, narrow dark lanes, small white-hot active
patches, and a red fringe. It deliberately moves away from the earlier broad
brown/gold quiet-region treatment. Earlier reference research also included:

- [ESA/NASA Solar Orbiter EUI mosaic](https://www.esa.int/ESA_Multimedia/Images/2025/04/Solar_Orbiter_s_widest_high-res_view_of_the_Sun):
  concentrated active areas, overlapping loops and uneven limb emission.
  Its broad dark-region contrast is not the palette/composition target for the
  current version.
- [NASA SDO prominence imagery](https://sdo.gsfc.nasa.gov/gallery/firstlight/):
  coherent, fuzzy plasma envelopes with finer threads inside them.
- [Solar Orbiter's visible-light view](https://www.esa.int/ESA_Multimedia/Images/2024/11/PHI_s_view_of_the_Sun_in_visible_light)
  was used as a contrast: the more subdued photosphere is not the dramatic EUV
  appearance this demo targets. [NASA explains the assigned EUV colors](https://www.nasa.gov/missions/sdo/nasas-sdo-sees-circular-outburst/).

These guide the procedural composition, not a physical radiative-transfer or
magnetic-field model. Runtime geometry and textures are still generated locally;
no reference photograph, measured solar data, or downloaded model is shipped or
loaded by the demo.

## Reproducible inspection

```sh
./build/vng_sun_demo --time 3 --screenshot /tmp/sun.png --analyze /tmp/sun-evidence
./build/vng_sun_demo --time 7 --screenshot /tmp/sun-later.png
./build/vng_sun_demo --time 60 --screenshot /tmp/sun-rotated.png
./build/vng_sun_demo --time 3 --no-displacement --screenshot /tmp/sun-smooth.png
./build/vng_sun_demo --time 3 --no-bloom --screenshot /tmp/sun-no-bloom.png
./build/vng_sun_demo --time 3 --white-spots --screenshot /tmp/sun-white-spots.png
```

Use new output paths: captures do not overwrite existing files/directories.
Either capture flag defaults to one frame at three seconds. An explicit
`--time` or `--frames` overrides that default. `--time` without a capture flag
freezes the sample while leaving the window open. `--width` and `--height`
accept integer dimensions from 64 to 8192; the default is 1280 by 800.

The final screenshot includes the HDR composition, corona, subtle screen-space
softening, bloom, tone mapping, and display encoding. Enhanced rendering provides
complementary unfiltered surface evidence rather than interpreting that screenshot:
displaced world position, radial surface direction (`SurfaceNormal`), and unclamped HDR
radiance can be inspected alongside surface identities, coverage, depth, and
the generated shader source. The observed normal is the normalized radial
direction used by this emissive shader, not a derivative-derived normal of the
displacement field. Enhanced captures isolate the surface; they exclude the
separately drawn corona and prominence strands as well as softening and bloom.

## Implementation boundaries

The sun renderer owns its geometry and internally creates its DSL shaders. A
draw supplies the changing time and displacement strength; no shader lambda is
replayed when time changes. The renderer computes one sun-local-to-world rotation
matrix and supplies the same typed `Mat3` argument to surface and arc shaders.
The sun turns around its +Y axis once per ten minutes of demo time; this is an
artistic playback speed, not a physical solar rotation period. Arc positions
and their glow cross-sections rotate together with the surface, including in
enhanced shader output. Sunspots remain fixed in sun-local UVs. Granulation and
displacement have only small bounded local motion, so texture scrolling no
longer carries landmarks past the arc roots.

These are typed runtime shader arguments, while the procedural pattern's fixed
tuning values are compile-time constants.
Time is reduced modulo 3,600 seconds before GPU upload to keep large finite
inputs numerically bounded. This is an hourly reset, not a seamless solar cycle.
The example is composed around a unit-radius sun at the origin viewed from
the +Z side. Its corona plane is authored for that view, rather than being a
general camera-facing celestial-object renderer.

Surface displacement samples the procedurally generated data texture in the
vertex shader at mip level two, filtering detail finer than the authored
sphere's vertex spacing, and moves sphere vertices along the
center-to-surface direction, changing the actual rendered geometry. It is not
only a scrolling color pattern on a smooth sphere. Turning displacement off
keeps the animated color pattern, making the geometric contribution easier to
compare. Displacement sampling fades to mip zero at the poles so their
longitude-independent base row keeps duplicate cap tips joined.
The same linear-data texture supplies fine granulation (R), fine
turbulence in short irregular tufts (G), spot/filament masks (B), and an active-region
emission envelope (A) to the fragment shader; it is not treated as an sRGB color
image. Fine surface detail, brighter active areas, and the surrounding glow
combine to make the close-up readable. The atmospheric forms and animation are
artistic approximations, not a magnetic-field or fluid solver.

The surface pattern uses mild local warping at a much shorter scale than the
old global swirls. It no longer includes the long contour/fan pattern in G;
the separate A/B fields still keep active regions and arc roots attached.
Spatial-correlation tests check that nearby samples remain coherent while
the pattern stops looking similar over larger distances. This changes the
displacement field as well as the colors; it does not add more mesh vertices.

The small engine additions are floating-point DSL `sin`, `cos`, `exp`, `pow`,
`abs`, `floor`, and `fract`; explicit-LOD `stage.sample_2d_lod<Slot>(uv, lod)`;
and backend-neutral `graphics.set(render::BlendMode::additive)`. Prominences
reuse one shader and mesh for a broader dim halo followed by a bright core,
with additive color blending and depth writes disabled. The 132 strands span
11 regions, distributed between major limb loops and smaller low-lying surface arcades.
Shared region definitions keep their emission footprints attached to their roots.
The soft sheath is ten times the core radius (up from seven), with lower peak
energy than the core, an orange/red tint and a smoother falloff. Its emission
was also strengthened to remain visible next to the brighter surface.
This broadens the glow without
brightening every strand into a hard wire. The corona is also less uniformly
outlined, with a diffuse red outer fringe, a tighter orange inner edge, and fine
short textured wisps.
Tube rings are spaced along curve length
at asset creation to avoid sliver triangles near
the apex of short, skewed arches; no mesh is rebuilt during animation.

Sunspot cores retain at least 50% of the local body emission before mild limb
darkening, so the narrow dark features do not become black stains. The example's `SunSoftening` helper
applies a small normalized 3×3
Gaussian filter to the complete linear HDR image before bloom and tone mapping.
Its screen-pixel footprint stays consistent on resize, gently softening tiny
granules and strand edges without flattening the larger surface structure.
This is separate from bloom and does not change other demos or diagnostic data.
The surface palette uses deeper red-orange lanes, bright orange midtones and
warm highlights, with a lower ceiling than the deliberate hot spots. A much
lower red floor preserves the darker lanes through tone mapping; a separate
body response keeps continuous color gradients without changing the response
inside authored hot spots. Sparse warm-yellow tips extend the range of the
brightest tufts without turning ordinary granulation white. White emission
comes only from anchored active-region footprints and three additional compact
surface knots; there is no unmasked white sparkle term on ordinary granules.
The footprints use a tighter falloff to keep the spots smaller, and their
emission is broken up by the fine surface texture. The three extra knots live
in the same sun-local data map, so they rotate with the surface without adding
meshes, draw calls, or new prominence strands.
One knot is positioned farther down the disk to separate it from the two bright
lower-left arcade footpoints. `SunDraw::white_spots` controls all added
white-hot surface emission, including in diagnostic captures; surface colors, dark
features, displacement, arcs, and corona remain. The live toggle uploads a typed
parameter, without rebuilding the map, meshes, or programs. Spots default to off;
`--white-spots` enables them initially, and `--no-white-spots` explicitly disables them.
Both flags work for reproducible screenshots.
It is no longer capped to the earlier ochre highlight range. The higher bloom
threshold preserves the dense surface detail while the existing broad strand
sheaths keep arcs soft. None of these appearance settings change the engine's
shared tone mapper or other examples.

`SunDraw` also accepts a world-space `position` and positive `radius` (defaults:
origin and one). Surface displacement and arcs scale together, radial normals
are measured relative to that center, and the corona uses the view's right/up
basis. Enhanced shaders receive the same placement and report world positions.
The [solar flyby](solar_flyby.md) uses this to share a camera and depth buffer with
the spaceship. It is still an example renderer, not a new engine scene abstraction.

Options, procedural assets, and rendering policy stay in example helpers.
Shared `support/presentation.hpp` owns final sRGB presentation and safe PNG
capture for both sun and spaceflight; neither demo depends on the other's
renderer or content. No sun-specific material system or render graph is
introduced.

See [typed shader arguments](typed_shader_arguments.md) for the argument model
and [spaceflight](spaceflight.md) for the shared display/capture path.
