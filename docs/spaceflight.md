# File-backed spaceship flyby

`vng_spaceflight_demo` loads an original spaceship model and a small scene
document, then flies the ship past a fixed camera against a star field. The
opening deliberately contains no ship: wait approximately five seconds for
it to enter. The complete 16-second shot loops.

```sh
cmake -S . -B build
cmake --build build --target vng_spaceflight_demo
./build/vng_spaceflight_demo
```

OpenGL 4.6 and GLFW are required; text/font support is not. Press `Space` to
pause, `R` to restart the shot, `B` to toggle bloom, and `Esc` to close.
Unlike the glow example, `R` restarts animation rather than reloading assets.

## Repeatable screenshots and diagnostics

The close-up inspection time is stored as `hero_time = 10.7` in the scene.
Requesting a screenshot or diagnostic export selects that time and exits
after one frame, unless an explicit time or frame limit was supplied.

```sh
flight_capture=$(mktemp -d /tmp/vng-spaceflight.XXXXXX)
./build/vng_spaceflight_demo \
  --screenshot "$flight_capture/hero.png" \
  --analyze "$flight_capture/hero-evidence"

./build/vng_spaceflight_demo --time 0 \
  --screenshot "$flight_capture/opening.png" \
  --analyze "$flight_capture/opening-evidence"

./build/vng_spaceflight_demo --no-bloom \
  --screenshot "$flight_capture/hero-no-bloom.png"
```

Screenshots refuse to overwrite existing files; evidence export requires a
new or empty directory. Use fresh destinations for subsequent captures.

`--once` alone renders the empty opening, not the close-up. To hold the
close-up in an interactive window, run `--time 10.7` without a capture option.
Other controls are `--frames N`, an optional first positional `.vscene` path,
and `--help`. `--no-bloom` disables the halo while retaining exposure and tone
mapping.

There are two different inspection products:

- `hero.png` reads the actual final window back buffer, including the stars,
  lighting, bloom, tone mapping and display encoding.
- `hero-evidence/` inspects only the ship, before bloom and tone mapping. The
  existing enhanced-render system emits diagnostic variants from the same
  shader IR and uses the same geometry, camera and draw transform. Production,
  culling-disabled and depth-always variants help distinguish missing geometry
  from visibility problems.

The evidence directory contains the engine's metadata and color, depth,
coverage, object-ID and face-ID visualizations, plus these example-specific
files:

- `normals.png`: world-space normals mapped from `[-1, 1]` to `[0, 1]`.
- `radiance.png`: an HDR radiance preview compressed with `x / (1 + x)`.
- `probes.csv`: floating-point world position, normal, radiance, device depth
  and surface identity at covered samples on an eight-pixel grid.
- `spaceflight.txt`: coverage counts and findings from the diagnostic sweep.

The canonical analysis color attachment is RGBA8 and can clip bright engine
values. The explicitly observed `Radiance` channel preserves their
floating-point values; the preview image is only a visualization. Probe image
coordinates start at the top left. An empty opening capture should have zero
covered ship pixels, even though its final screenshot contains stars.

## Scene data stays application-specific

[spaceflight.vscene](../examples/assets/spaceflight.vscene) uses the generic
[document API](documents.md); `flight`, `background` and `bloom` are conventions
of this example, not concepts built into the parser.

```text
vscene 1.0

mesh = "spaceship.vmesh";
hero_time = 10.7;

flight = {
    start = [0, 0, 40];
    end = [0, 0, -20];
    duration = 16;
    loop = true;
    bank_degrees = 4;
};
```

The full document also sets the camera, viewport extent, deterministic star
seed and bloom parameters. Relative mesh paths are resolved against the scene
file's directory. Unrelated fields such as editor notes are not interpreted.
Malformed known values still produce source-aware diagnostics.

Flight interpolates position between the endpoints and adds a subtle rigid
bank. The mesh points along local `-Z`, with `+Y` up. Animation updates a
`ShipDraw` transform; it does not rebuild the mesh, shaders or star field.
The ship vertex lambda takes a `Float4x4 model` argument and its fragment lambda
takes `Expr<Lighting>` (eye, key-light source and color). The renderer supplies the
ordinary `Mat4` and a typed CPU record. No scratch buffer or manual matrix packing is needed; enhanced
variants receive the same copied argument values as production rendering.

An optional `flight.controls` pair supplies cubic Bezier handles. That path
orients local `-Z` along its tangent and banks into the turn; the original
endpoint-only path is unchanged. An optional `sun` section adds the procedural
sun and positions the ship's warm key light there. See [solar flyby](solar_flyby.md).

## Model and renderer

[spaceship.vmesh](../examples/assets/spaceship.vmesh) is the original
KESTREL / K-07 interceptor: 7,154 triangles and 21,462 flat-shaded vertices.
It has segmented ivory armor over a graphite hull, a faceted canopy, swept
wings, detailed twin nacelles, fins, vents, hatches and geometric markings.
Every vertex provides position, geometric normal, linear color and emission.
Lighting is evaluated in the shader, not baked into the vertex colors.

The offline [model generator](../examples/tools/make_spaceship.cpp) makes the
authored geometry reproducible. It is excluded from the default build and is
not run by the demo:

```sh
cmake --build build --target vng_make_spaceship
./build/vng_make_spaceship /tmp/kestrel-regenerated.vmesh
```

The tool writes the path explicitly supplied by the caller; choose a new path
unless replacing an asset intentionally. To use edits to the bundled scene
or mesh, reconfigure CMake so the build's asset copies are refreshed, or pass
the source scene explicitly:

```sh
./build/vng_spaceflight_demo examples/assets/spaceflight.vscene
```

`ShipRenderer : opengl::Renderer<ShipDraw>` owns its mesh, GPU resources and both DSL
shader definitions. Its factory creates the shaders internally. The fragment
shader supplies directional lighting, a cool fill, a small specular/rim term
and emission. The main example only supplies its view and draw ticket.

The output sequence is explicit:

```text
star field + ship -> linear HDR target
                 -> bloom + tone mapping -> sRGB color target
                 -> encoded-pixel copy to the window -> screenshot/present
```

The offscreen sRGB target performs display encoding once. The example's
OpenGL presentation helper copies those already-encoded values to the linear
default framebuffer without encoding them again; it does not rely on the
window system supplying an sRGB-capable default framebuffer.

## Where to look

- [spaceflight.cpp](../examples/spaceflight.cpp): setup, tickets, frame sequence
  and optional inspection.
- [spaceflight_scene.cpp](../examples/support/spaceflight_scene.cpp): typed
  document decoding, scene-relative paths, option parsing and animation.
- [spaceflight_assets.hpp](../examples/support/spaceflight_assets.hpp): the
  four string-to-semantic `.vmesh` mappings.
- [spaceship_renderer.cpp](../examples/support/spaceship_renderer.cpp): both
  ordinary DSL shaders, resource ownership and enhanced rendering.
- [space_background.cpp](../examples/support/space_background.cpp): cached
  star/dust geometry for this fixed-camera shot.
- [presentation.cpp](../examples/support/presentation.cpp):
  shared sRGB display surface, state-preserving window copy, screenshot readback and PNG writing.
- [spaceflight_inspection.cpp](../examples/support/spaceflight_inspection.cpp):
  ship-specific diagnostic visualizations and export.

Model and scene tests are CPU-only:

```sh
ctest --test-dir build \
  -R 'vng_(spaceship_model|spaceflight_scene)_tests' --output-on-failure
```

This is a focused example, not a general scene engine. The flight is rigid,
the lighting is not PBR, and the engine plumes are emissive opaque geometry,
not particles. No textures, gameplay simulation or new scene-management
abstraction are required for the shot.
