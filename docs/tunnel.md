# Inside the skyway

Run `./build/vng_tunnel_demo` for the express-departure cinematic, or open the
same editable scene:

```sh
./build/vng_editor_demo --scene examples/assets/tunnel_departure.vscene
```

One world unit represents **one kilometre**: the interior diameter is 6 km,
the route is 1,000 km long, and its bend follows an Earth-sized radius.

## Express departure (116 seconds)

The skyway act is the first 19 seconds; the voyage into space follows it (see
[The voyage](#the-voyage)). The courier starts about 141 km before the exit,
overtakes heavy transports, patrols and shuttles, and passes through the
shared Earth dispersal terminal.
Its starting speed is 3.432 km/s (another 20% increase). Approach acceleration is 40% stronger
and its envelope starts two seconds earlier:
`3.432 + 16.8*(smoothstep(min((t+2)/40,1)) - smoothstep(2/40))`.
It still levels off smoothly. Nearing the exit the courier opens up: from
18.0 s it surges by 120 km/s in 0.8 s, about ten times its speed, so the last
kilometres of the tube and the whole terminal flash past in half a second. The
camera stays locked to it throughout, and the climb then eases back toward
70 km/s over eight seconds. The terminal now adds only an **8-degree** incline.
Just after the mouth the voyage takes over the courier and the camera from
exactly the skyway's last position, velocity and framing. This is an authored
cinematic trajectory, not an orbital mechanics simulation.

The route is roughly 159 km above sea level, above the elevated
stylized terrain. The Empty Quarter in Arabia (around 52° E, 22° N), with its
paired express lanes, long-haul crossings and five gateway arcologies, lies beneath the
departure. Earth uses `earth_future.vmesh`, retaining editable settlements,
lights, skyways and launch hubs, not the undecorated planet.

One ordinary animated scene-camera instance films the skyway. It opens on the
empty bore: the camera waits just above the courier's line, looking down the
tunnel, and drifts a little, rising as it turns, before it settles. Traffic
comes in from the edges: a heavy transport descends into view overhead, then a
patrol and a shuttle. At 5.6 s the courier, ten times faster, dives in under the
camera and away down the middle, past them all; the operator's aim eases
toward it once. At 6.1 s the camera races after it: from rest it closes
kilometres in half a second, past the slower traffic, and eases into the close
chase at the courier's own speed. The traffic further down the tube starts beyond the
haze, so the opening's bore reads empty. Just before the throat, one eased move
brings the camera closer and slightly to the side. These are actual camera
moves, not cuts or crossfades, and never pass through the ship.

| Time | Shot |
| --- | --- |
| 0–2 s | The empty bore; the camera drifts and settles |
| 2–5.6 s | Traffic comes in: a heavy transport overhead, a patrol, a shuttle |
| 5.6–6.1 s | The courier dives in under the camera and away past them |
| 6.1–6.6 s | The camera races after it, past the traffic, into the close chase |
| 6.6–17.07 s | Follow the ship past traffic |
| 17.07–19.39 s | Closer side-offset exit approach; the surge from 18 s; clear the mouth at ~18.59 s |
| 19.39 s– | The voyage: pull up into the sky (below) |

Positions, headings, lenses and visibility are serialized timeline tracks;
there are no playback callbacks hidden in the demo. Keyframe names mark story
beats; intermediate motion samples remain editable. The geometry recipes and
shot-authoring helpers live in `examples/scenes/tunnel_departure.cpp`.

The original slow interior study is preserved:

```sh
./build/vng_tunnel_demo examples/assets/tunnel_interior.vscene
```

That 36-second study stays deep inside the route, where curvature hides the
remote portal. The departure version approaches the actual opening.

In the slow study the shell and collars remain separate demonstration meshes.
In the departure cinematic, the tunnel, collars and exit are part of the Earth
blueprint itself: **Arabian express / cinematic local**. There is no separate
cinematic shell or terminal instance. Camera and craft movement use normal
timeline tracks. Select a keyframe before editing animated instance transforms.

## The voyage

After the mouth the courier climbs into orbit, threads an orbital gateway and passes under the catcher for the Moon's mass-driver cargo,
burns for the Moon, skims its night side past a mining base into sunrise, runs
an asteroid belt, meets a fleet as it warps in, takes station beside the
flagship, and jumps away with it, leaving home in view. The authoring code is
`examples/scenes/departure_voyage.cpp`; the offline geometry (Moon, lunar base,
gateway, ring, glows, fragments, plumes) is in `examples/support/space_assets.cpp`.

| Time | Shot |
| --- | --- |
| 19.4–30 s | Pull up out of the terminal and climb past the arcologies; swing to a side view on Earth's limb |
| 30–32.8 s | Earth and the orbital gateway, a freighter crossing in front |
| 32.8–39.4 s | Threading the spinning habitat ring below the hub |
| 39.4–41.6 s | The mass-driver catcher: its lit funnel mouth faces the Moon, the ring behind it |
| 41.6–50.3 s | Under the catcher; round behind the courier as it turns for the Moon; ignition, and it burns away toward a half-lit Moon as the camera coasts to rest and the lens lengthens |
| 50.3–55.3 s | Down to the lunar night, the sun setting behind the limb |
| 55.3–59 s | Earthrise over the Serenity mining works; the courier roars in over the camera and away under Earth |
| 59–64.3 s | Low along the mass driver's gantry lights |
| 64.3–72.5 s | Lunar sunrise as the courier climbs away |
| 72.5–74.9 s | The belt's edge: the courier threads in past a big rock |
| 74.9–86.8 s | Chase, top-down, then leading the courier head-on until it overtakes the camera |
| 86.8–97.6 s | The clearing: the courier brakes into frame; the fleet warps in, far to near, then the flagship; the courier sets off to join it |
| 97.6–99.9 s | Taking station beside the flagship |
| 99.9–116 s | In formation, drifting back until home is in view; the fleet jumps away, the flagship, then the courier; home |

**Staging.** Everything stays in kilometres, but the scene's 32-bit keys cannot
hold both a 110,000 km Earth-Moon distance and metre-scale camera work. Each
location (Earth orbit, the Moon, the belt) is therefore staged around the scene
origin, rotated so craft never head along the X axis, where Euler angles are in
gimbal lock. Earth, the Moon (with its base) and the sun are placed in each
location's frame at time zero and at the two location cuts, so they jump only
where the picture cuts. The sun sits 900,000 km away, and a bright white core
glow makes it blaze through bloom. Like any film, some shots cheat the light,
at a cut or under a camera move: the sun swings to the camera's right during
the turn for the burn so the Moon shows its terminator; it sits 6 degrees
higher as the courier arrives at the Moon and sinks back as it dives; it moves
20 degrees left for the lunar sunrise, to the side for the belt's rocks and
arrivals, and behind the camera for the formation and home. The lunar base's
flyby cheats the courier's path the same way, between two cuts.

**The Moon** is one mesh made of three nested grids that share one relief
function: 2.5 degrees everywhere (with polar caps that halve their columns
toward the poles), 5 km across the flight corridor's patch, and 0.6 km along
the low-level strip. Each finer grid fills a hole in the coarser one, and its
rim lies exactly on the coarser triangles before easing onto the true surface,
so there are no seams or skirts. Craters come in three sizes, each only where
its grid can resolve it, with a band of middling hollows around the strip; the
near side carries the familiar maria and a faint earthshine on its night side.
It uses the `regolith` lighting style. The mining base is a separate blueprint
built on a graded site in the Moon's own coordinates.

**The fleet** is laid out by bearing and distance as seen from the clearing
camera, so the arrivals spread across that frame, far to near. An arrival is a
point of light, then a white needle whose head rides the nose while the hull,
stretched out behind it, contracts and coasts into place. A departure brightens
the drives, stretches the hull ahead from its tail and sends a needle racing
off, with a thin flash along the way it went; the jumps run from the back of
the formation to the front, then the flagship, then the courier alone. All of
it is ordinary keys on ordinary instances (position, rotation, scale, axis
scale, brightness and visibility).

**Camera.** The camera instance's eye is keyed directly, never rebuilt from a
distant orbit pivot (that rounding was the old post-tunnel shake). Moves that
follow fast, accelerating craft are keyed densely, samples that land on a cut
become exactly the cut, and chase frames that could roll or snap with a banking
craft use a level frame and a smoothed heading. The scene test checks that the
courier's screen position stays smooth frame to frame at 60 fps after the tunnel
exit, through the lunar sunrise, through the belt, and while it takes station
and holds formation; that craft always fly nose first and upright; and that the
courier is in frame through every act. The demo plays the camera through
`editor_example::render_camera`, which uses the scene's
`environment.view_distance` (1,200,000 km here) for the far plane. The editor
uses its own **Settings → Maximum viewing distance** instead; raise it to at
least 1,200,000 to see the sun, Earth and Moon from the other locations.

**Budgets.** The generated scene is about 47 MB, about 56 MiB decoded against
the editor's 64 MiB document limit, and uses about 11,500 of the timeline's
16,384 keys: collinear camera and courier keys (static shots, holds, straight
runs) are simplified away, which pays for dense keys on fast moves.

## One tunnel, viewed from either side

`earth::detail::tunnel_shell(span<TunnelSection>, light)` and `tunnel_collar(frame, size)`
in `examples/support/earth_structures.hpp` are CPU-only geometry recipes shared
by Earth infrastructure and both tunnel scenes. A section supplies a route frame
and size; the close-up route supplies more samples, not a different tunnel design.
The original six-sided profile, dark ribs and cyan crown remain. There are now
separate outer and inner surfaces with a thin physical wall between them, and
emissive cyan/amber lanes on the inside. Six kilometres measures the clear
vertical height; this is a hexagonal passage, not a circular bore.

The cinematic uses **Local** from `earth_tunnel_sizes.hpp`, the same 6 km class
available on the Earth blueprint (6371 km reference radius). Regional routes
have a nominal 24 km bore; trunk routes about 45.4 km, exactly 40% narrower than
the former standard. Blueprint and per-part multipliers still apply to Earth
routes. Their length and arch height are independent of the new size class.
This is one passage, not yet a tessellated bundle of smaller passages.

The departure author reads the actual Earth route and samples its curve and
terminal for ship, traffic and camera tracks. Open Earth's blueprint mesh editor
and select **Pacific express / cinematic local** to edit it just like any tunnel.
After reshaping the route, regenerate the cinematic to refit its baked timeline;
existing animation keys do not automatically become a live path constraint.

Nearby local highway routes are independent lanes with optional Bézier paths.
Choose **Tunnel path → Bezier control points**, then cycle to that gizmo. Its
options expose **Line segments (8–256)** and **Add control point (preserve shape)**;
pick a point to move it over Earth or radially, or delete it. A/B remain the normal
endpoint handles (or structure sockets). Explicit curves are not automatically
lifted out of terrain, nor made exactly parallel to neighboring lanes.

The ordinary instanced mesh renderer draws the complete tunnel. Depth testing
chooses the visible surface, including views through the mouth where exterior
and interior are visible simultaneously. No whole-ticket inside/outside routing
or duplicate renderer resources are needed. Indexed rings share route samples
to keep geometry compact. Dense Earth presets with real tunnel linings use a
bounded 131,072-vertex blueprint budget (32-bit indices), rather than reducing
their existing route tessellation. Import, subdivision, selection and vertex
patches share the corresponding editor limit. The close-up collars are spaced 6 km apart.

Scene regeneration embeds the authored `earth_future.vmesh` unchanged, including
its class choices and hand edits. Previously saved Earth meshes keep their baked
geometry until explicitly rebuilt or retuned; there is no hidden rebuild when
authoring the cinematic, during playback or during camera movement.

The cyan/amber lamps use the existing emission stream and HDR bloom. Local wall
illumination is baked into vertex colors; a `render/lighting = tunnel` mesh
material adds per-pixel, camera-relative distance haze in kilometre units, with
bright near-white spill extending onto the walls. This
is a stylized lighting approximation, not dynamic volumetric scattering or
shadow-casting local lights. Other scene materials are unchanged.
The departure variant uses `tunnel_departure`, whose exit is at Z=0 with the
tube along +Z. A depth-tested veil across the mouth renders **after** opaque
geometry, hiding distant Earth/addons as well as stars. Nearby walls and craft
occlude it. Its opacity approaches white exponentially with distance, then
fades continuously during the approach; it never writes depth and is disabled
for exterior views. Shorter interior camera focus and
analytic ship headings also avoid the old large-coordinate rounding shake.
Earth retains its own illustrated material.

```sh
cmake --build build --target vng_tunnel_demo vng_make_tunnel_departure vng_tunnel_scene_tests
./build/vng_tunnel_demo --time 7 --screenshot /tmp/tunnel.png
./build/vng_tunnel_demo --time 23 --analyze /tmp/tunnel-evidence
./build/vng_tunnel_demo --time 57.9 --screenshot /tmp/serenity.png
./build/vng_tunnel_demo --time 104.5 --no-bloom
```

Capture paths must be new. The authoring recipes live in
`examples/scenes/tunnel_scene.cpp`, `tunnel_departure.cpp` and
`departure_voyage.cpp`; regeneration is explicit and never happens when
launching the demo:

```sh
./build/vng_make_tunnel_departure examples/assets /tmp/tunnel_departure.vscene
./build/vng_make_tunnel_scene examples/assets /tmp/tunnel_interior.vscene
```

Use `--replace` only to deliberately replace an existing generated scene.
