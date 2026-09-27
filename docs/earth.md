# Earth / stylized homeworld

See [visual direction](art_direction.md) for the broader game-wide style target.

## Connected homeworld (futuristic variant)

`examples/assets/earth_future.vmesh` is a separate, importable Earth blueprint.
It retains the original terrain and authored clouds, adding warm clustered
city lights, low sweeping graphite skyways and compact gateway arcologies.
The original Earth and savannah files are not changed.

The current composition keeps the cities and replaces the old scattered addons
with four paired corridors (Pacific Asia, Arabia–India, Mediterranean and western
America), the cinematic express route and two neighboring lanes. These eleven
tunnels use explicit editable Bézier controls, sparse supports and selective flared ends.
Five modestly scaled launch hubs build up the Arabian departure region; the old giant loops, standalone
elevators, terminals and unrelated structures are removed from this composition,
not from the palette of available blueprints. The express route retains its
flight-compatible shape and is still part of the Earth mesh.

The global extension adds twenty-five oceanic/polar/continental corridors and thirteen
compact gateways, for **36 tunnels and 18 hubs** in total. The long-haul routes span
roughly 2,000–6,000 km, with paired lanes, meridian spines and a dense Arabian
crossroads. The 6 km-bore cinematic local emerges over the Empty Quarter near
52° E, 22° N. Its editable **Mouth incline** is 8 degrees; existing tunnels retain
their 30-degree default unless edited. Both hemispheres, including
Pacific-facing and polar views, have visible infrastructure. These outer
corridors have editable Bézier controls too; they use 24 segments and three
supports each instead of the close-up express's denser geometry.

To add that extension without replacing any existing parts or hand edits:

```sh
./build/vng_make_earth SOURCE_DIRECTORY OUTPUT_DIRECTORY --global-infrastructure
```

This is also idempotent and supports explicit `--replace`. Newly generated
`--future` assets include both the coastal composition and the global extension.
The supplied scene keeps its overlapping spare Earth instance hidden: enabling
two co-located planet instances causes intersecting terrain and z-fighting,
not a terrain-texture problem. Move the spare away before making it visible.

The preview runtime uses reversed floating-point depth for kilometre-scale
scenes, including the shared annotation depth attachment. This preserves nearby
ships and distant terrain without changing camera clipping, position or zoom.
`render::FrameDesc::depth_mapping` selects the encoding; logical clears and
`DepthCompare::less` keep their usual meaning. OpenGL translates camera
projection, comparisons and clears behind that neutral interface. Diagnostic
captures use the same mapping and return their documented canonical depth
channel. This does not hide genuinely overlapping instances.

To deliberately apply this reset to another connected-Earth copy:

```sh
./build/vng_make_earth SOURCE_DIRECTORY OUTPUT_DIRECTORY --redesign-infrastructure
```

Add `--replace` to replace existing outputs. This preserves city identities and
baked city/terrain/cloud geometry, but removes all other existing addons before
placing the new network. The step is idempotent and never runs on scene load.

```sh
./build/vng_earth_demo examples/assets/earth_future.vscene
./build/vng_editor_demo --scene examples/assets/earth_future.vscene
```

The scene has a slow, editable orbit from dusk into night. City emission fades
in smoothly at the terminator, following the scene's light direction (or Sun
instance), not the viewer. Cool night-side shadows keep the continents readable.
Skyways follow gently curved regional corridors, roughly 700–1,800 km long; width
and altitude are intentionally exaggerated to read from orbit. These are
illustrated settlements and transport infrastructure, not accurate city maps.
The tunnels have substantial dark shells, raised near-black structural collars,
endpoint piers and narrow cyan crown strips interrupted by the ribs. Launch
hubs have stepped hexagonal towers, elevated docking decks, warm window bands
and satellite towers. Their silhouettes are deliberately larger and taller
than realistic orbital-scale buildings, retaining the illustrated art style.

More placeable megastructures are available in **Blueprint geometry**:

- **Dispersal terminal**: a narrow throat widening into a hollow, armored fan,
  with dark ribs, three exit lanes, shoulder towers and sparse navigation lights.
- **Orbital elevator**: twin inclined pylons, braced suspended decks, docking
  fingers and a thin tether rising to an upper station. Its silhouette is taller
  and narrower than the launch hub.
- **Tunnel joiner**: an elevated, hollow Y chamber with an inlet and two output
  sockets, dark port collars and grounded cross-braced supports.

Select a skyway and enable **Dispersal terminal at A/B**, then **Apply part
properties**, to attach a terminal to either end. This is part of the tunnel's
recipe: it follows both endpoint gizmos and whole-part transforms automatically,
without a second scene instance or an independent attachment to keep aligned.
The widening section follows the tunnel curve and replaces its end section;
the mouth has real inner walls and rim thickness, not a painted black cap.

On **any Earth blueprint**, open mesh edit → **Blueprint geometry** →
**Earth infrastructure / mesh draft** (select the whole Earth, not a cloud):

- **Night city lights**, **Raised skyway tunnels**, **Launch hubs**: independent toggles.
- **Terminals / joiners / elevators** controls standalone megastructures; attached
  terminals follow their parent skyway's visibility instead.
- **Infrastructure brightness**, **City density**, **Skyway height**: multipliers, initially 1.
- **Structure size** controls tunnel thickness and building footprints (0.5–2);
  **Building height** stretches hubs/elevators and raises standalone terminals
  (0.5–3). Both default to 1; each part also has its own size/height controls.
- **Apply infrastructure** rebuilds just this authored layer in the background,
  as one undoable draft edit. Then **Apply to scene** publishes the draft;
  saving to disk remains a separate operation. Existing Earth assets start with
  these optional features off. The connected-homeworld preset enables all three.

Mesh edit's camera-following work light is meant for authoring; use scene view
or the demo to inspect night lighting. Cloud rebuilds/movement preserve the
infrastructure; rebuilding infrastructure preserves terrain, cloud placements,
custom vertex fields and the baked whole-mesh authoring frame. Hand edits *to
the generated infrastructure itself* are replaced on rebuild. Faces/edges that
join it to unrelated geometry must be separated first.

### Place and manipulate infrastructure

Earth owns local instances of six recipes: **settlement**, **skyway**,
**launch hub**, **dispersal terminal**, **orbital elevator** and **tunnel joiner**.
Think of these as small blueprints inside the Earth blueprint:
each has a stable identity, name, placement, size and visibility, but none is a
separate scene instance. All scene instances of this Earth share its authored
infrastructure, just as they share its terrain and clouds.

In Earth's mesh view, open **Blueprint geometry** and choose **Whole blueprint**:

1. Click a **Place…** button (including **Place dispersal terminal** and
   **Place orbital elevator** and **Place tunnel joiner**).
2. Click the visible Earth surface. **Esc** cancels placement; clicking empty
   space leaves the placement tool active. The new part is selected immediately.
3. **G** or drag its surface handle to move along Earth; **R** or its heading
   ring rotates it. Arrows and the standard sensitivity control also work.
   **Enter/LMB** confirms a keyboard gesture; **Esc/RMB** restores its start.
   Terminals, joiners, launch hubs and elevators also show an **Up / down** axis
   in their normal **Whole part** gizmo. Drag it or select it and use arrows.
   The structure's sockets and attached tunnels follow; optional support feet
   remain on the ground. Surface dragging preserves the chosen altitude.
4. For a skyway, **Gizmo** starts at **Whole part**. Choose **Tunnel endpoints**
   (or cycle with **Ctrl+Left/Right**, using either Ctrl key) to show both
   **Endpoint A** and **Endpoint B**. Drag either handle directly;
   the other anchor stays fixed while the tunnel reshapes. **Handle** chooses
   which end responds to G/arrows. Whole-part movement/rotation carries both
   anchors together; selecting another part restores the default whole-part gizmo.
5. **Width / footprint**, **Height / rise**, **Altitude (Earth radii)**,
   **Part visible** and **Part name** stage locally;
   **Apply part properties** commits them to the mesh draft. **Remove part**
   removes only that part. Undo/redo covers creation, gestures and removal.

**Gizmo → Scale part (S)** controls the individual part multiplier (1 by default).
Cycle to it with **Ctrl+Left/Right**, then drag the square handle or press **S**
and move the mouse. Arrow keys respect sensitivity; Enter/click confirms and Esc
cancels. The bottom-left options also provide **Part scale** and **Apply part scale**.
Scaling is one undoable edit, separate from the blueprint baseline. A standalone
structure scales uniformly about its ground-level anchor, including ribs, hull
height, length, width and socket positions. The width and height sliders remain
independent shape modifiers. Tunnels retain their endpoint/socket positions:
overall size multiplies thickness, fittings and arch rise, not the distance
between their anchors. Settlements scale their surface footprint.

### Addon baseline scales

Open **Addon baseline scales...** at the top of Earth's mesh-edit controls.
The button opens/closes a compact menu, including while a cloud or structure is
selected. It has a global **All addons / baseline scale**, plus coefficients for
settlements, tunnels, launch pads, dispersal terminals, orbital elevators, tunnel
joiners and atmospheric processors. Each ranges from **0.1 to 4**, defaults to 1,
and accepts typed values as well as slider input.

The effective multiplier is **baseline × type coefficient × Part scale**.
Existing width/height controls remain additional shape modifiers. Standalone
structures scale around their surface anchor; altitude is not multiplied. Tunnel
width and arch height scale, but their authored surface endpoints stay put.
Connected endpoints follow their resized sockets, including the socket width.
Tunnel-owned terminal fittings follow their tunnel's scale; separately placed
dispersal terminals use the terminal coefficient.

Values stage locally until **Apply addon scales**. Apply regenerates the addon
layer, preserving terrain/clouds, part identities, locations and individual size
settings. Generated-addon hand edits are replaced, like **Apply infrastructure**.
Undo restores the previous geometry/settings. **Apply mesh to scene** publishes
the draft; saving persists its multipliers. Older assets without these settings
load with identity multipliers.

### Atmospheric processors

Choose **Place atmospheric processor**, then click Earth's surface. This
non-transport addon has three open intake towers, exposed dark ribs, recessed
grates, cooling banks and sparse cyan/amber lights. It uses the existing part
selection, surface move, heading rotation, altitude, per-part size, visibility,
optional ground scaffolding and removal controls. **Atmospheric processors** in
the whole-blueprint infrastructure controls toggles the type without deleting
its saved parts. No processor is automatically added to existing scenes.

### Scaffold placement

Scaffold controls live in the active gizmo's **bottom-left options**, not the
sidebar. **Show scaffolding** is independent for each part: hiding ground supports
does not remove the hull, ribs, an elevator's intrinsic pylons, or saved positions.
Standalone structures expose this toggle in their whole-part gizmo options.

Select a tunnel and choose one of these modes in **Gizmo**, or cycle with
**Ctrl+Left/Right**:

- **Scaffold positions**: pick a support marker (or select **Handle**), then drag
  along the highlighted route. **G** and sensitivity-scaled arrow keys work too.
  **Delete** or **Delete Support N** in the options removes that support.
- **Add scaffold**: move the **New support** marker along the route, then click
  **Add support here** in the options. Moving the marker alone changes no geometry.
- **Uniform scaffold spacing**: available only when there are no interior supports
  (start/end supports may remain). Move **First interior support**, **Last interior
  support**, and **Spacing**, or edit their option values, then **Apply uniform
  spacing**. Supports are equally spaced by arc length between the exact chosen
  first/last positions. The requested spacing is a maximum; intervals are adjusted
  slightly to fit both ends. Existing endpoint supports are retained. The gizmo
  disappears after Apply because interior supports now exist.

**Reset to endpoint supports** clears manual interiors and restores start/end
supports, making uniform setup available again. There is a maximum of 64 supports
per tunnel; excessive density rejects Apply with a message. Spacing is in Earth
radii (one radius is about 6,371 km). **Esc** cancels gestures; undo/redo covers
move, delete, add, reset and uniform Apply. Positions follow endpoint/socket edits.
These are blueprint draft edits: **Apply mesh to scene** publishes them; saving
retains them on disk.

In **Surface (4)** mode, click a visible structure to select it, or choose its
name in **Edit part**. Actual mesh geometry determines picking and occlusion.
Click terrain/empty space outside the gizmo, or press **Esc** while idle, to
clear the part selection. During a transform, Esc cancels it first; another
Esc clears selection. A viewport click confirms an active keyboard transform.
Choosing a part in the list automatically enters Surface mode. **Whole blueprint**
returns to the global cloud/infrastructure controls. Placement automatically
enables that infrastructure layer if needed.

These edits stay in the mesh **draft** until **Apply mesh to scene**. Saving is
still separate. The `.vmesh` stores the recipes as well as their baked geometry;
reopening or regenerating a layer retains additions, removals and placements.
Existing older futuristic meshes expose **Enable infrastructure editing (rebuild)**
for an explicit one-time migration. This regenerates infrastructure, so apply it
before making manual vertex edits to those structures. The supplied futuristic
preset is already migrated.

Moving a part along the surface or rotating it transforms only its owned positions/normals, preserving
hand edits and custom fields. Connected tunnels are regenerated as dependents;
unrelated geometry stays untouched. Endpoint/property edits regenerate only that part
and any connected tunnels;
when its topology is unchanged they retain indices and use sparse GPU updates.
The editor runs CPU edits on its existing background draft-job path and shows
the pending gizmo/spinner until the worker presents them. Add/remove operations
can change topology and therefore require a new mesh upload. Global **Apply
infrastructure** deliberately regenerates the entire infrastructure layer.

### Connect tunnels to structures

1. Place a **Dispersal terminal** or **Tunnel joiner**, plus a **Skyway**.
2. Select the skyway in surface mode (**4**), then **Attach endpoint A/B in viewport**.
   Click a terminal or joiner: its available socket handles appear. Click the
   terminal's **Entrance**, or the joiner's **Inlet**, **Left outlet** or **Right outlet**.
   Only the final socket click edits the draft; the source tunnel stays selected.
   **Esc**, RMB or **Cancel attachment** exits without making a connection.
   The **End A/B connection** dropdowns remain an alternative.
3. Connect other skyways to the remaining sockets to build the fork. Each socket
   accepts one tunnel end; occupied sockets are omitted from other end choices.

The attached end leaves its ground anchor, matches the socket position/direction
and thickness, and gains ground supports beneath its raised approach. Moving,
rotating or resizing the structure reshapes its connected tunnels automatically.
These are geometric connections, not a traffic simulation.

Choose **Free endpoint (detached)** to disconnect. The endpoint stays at its socket's
position and height. Removing a structure similarly frees its ends without deleting
the tunnels. In **Tunnel endpoints** gizmo mode, select A/B, then drag its **Up / down**
axis to change altitude, or drag its center to move along Earth at that height.
The axis stays selected for arrow-key control. Numeric **End A/B altitude (Earth radii)**
fields are also available under **Apply part properties**. Raised free ends keep
ground scaffolds; further height changes patch the existing geometry.
Attached ends cannot be dragged away accidentally: only
free endpoint gizmos remain. Whole-tunnel move/rotate returns after both ends are
detached. You can still change tunnel height/size while attached. Hiding a structure
does not remove its socket references; its tunnels remain independently visible.

**Dispersal terminal at A/B** is still available for unconnected ends when you
want a simple fitting owned by the tunnel. Connecting that end to a standalone
structure replaces the fitting. Apply to scene / Save remain separate, as usual.

### Tunnel shape

Select a tunnel part in blueprint geometry to use **Tunnel size class (nominal bore)**:

| Class | Nominal clear vertical bore |
| --- | --- |
| Local | 6 km; also used by the tunnel cinematic |
| Regional | 24 km |
| Trunk | About 45.4 km; 40% below the previous standard |

These sizes share a 6371 km Earth-radius reference. Width/footprint, part scale,
and addon/tunnel baseline multipliers still multiply them. Changing class only
changes cross-sectional dimensions, not endpoints, route length or arch height.
Socket-connected ends retain their socket dimensions and taper to the class bore.
Older recipes default to trunk when rebuilt. Existing baked assets are not
silently regenerated just by opening a scene.

The size-class system supports all three classes. The current focused Earth
composition uses local and regional routes; trunk remains available in the editor.
The earlier additive network (four local feeders, nine highway lanes and the
cinematic route) can still be authored separately. Its explicit, idempotent command
can apply the same update to a copy of an older connected Earth, preserving its
terrain, clouds, other structures and existing route endpoints:

```sh
./build/vng_make_earth SOURCE_DIRECTORY NEW_OUTPUT_DIRECTORY --tunnel-classes
```

Add `--replace` only when deliberately replacing existing output files. Ordinary
infrastructure rebuilds preserve the authored catalogue and class choices.
The possible future interpretation as bundles of smaller tessellated tunnels
is conceptual only; it is not implemented.

Unconnected endpoints store longitude/latitude plus independent altitude. `SkywayCurve`
converts them to unit directions A and B and samples the shorter great-circle
route at constant angular speed, with a radial arch:

```text
P(t) = ((1−t) × Rₐ + t × Rᵦ + h × sin(πt)) × slerp(A, B, t),     0 ≤ t ≤ 1
Rₐ/Rᵦ = 1.024 + altitude A/B (Earth radii)
h = 0.060 × global Skyway height × Height / rise × baseline × tunnel coefficient × Part scale
```

Each endpoint stays at its own radius; arch height does not change its position.
The tube, lighting strips and dark ribs share this curve and its analytical
tangent, so the cross-section stays perpendicular to the path. **Width / footprint**
controls thickness independently. Routes work across the longitude seam and
poles; anchors must be 0.1–150 degrees apart to avoid degenerate routes. Invalid
edits are rejected without replacing the current mesh. Fixed tessellation keeps
endpoint moves on the existing sparse-update path.

`TunnelCurve` adds socket-bound endpoints. It uses a cubic Hermite route with
outward/inward socket tangents, a smooth radial arch (zero slope at the ends),
and terrain clearance. Tube frames align with the socket frames; width blends
from socket size to the tunnel's own size. Connection changes may change topology;
subsequent placement/size/height edits retain it and use sparse vertex uploads.

### Explicit Bézier paths

Select a tunnel, choose **Tunnel path → Bezier control points**, and cycle to
the **Bezier control points** gizmo. Select an interior point to move it around
Earth or radially with Up/down. Delete removes the selected control; the gizmo's
menu offers **Add control point (preserve shape)** and **Line segments (8–256)**.
Adding uses degree elevation, so the existing path does not jump. Deleting
reshapes it. The regular endpoint gizmo still edits A/B; connected endpoints
remain attached to their sockets. This mode leaves interior controls fully
authored, including their approach direction and clearance from terrain.

`InfrastructurePart::bezier_controls` contains canonical Earth-space interior
points (up to 14). An absent value selects the original automatic path; an empty
list gives a straight segment. `TunnelCurve` evaluates de Casteljau; geometry
samples it into `curve_segments` sections. Whole-part moves/rotations carry its
controls along. Only the affected part's mesh is rebuilt. Changing segment or
control count is undoable; all data persists in recipe version 9. Versions 1–7
remain readable. The editor host owns no tunnel-specific state.

Terminals now consume a length proportional to bore size (up to 18% of short
routes), rather than always occupying 18% of an intercontinental tunnel.

### Code ownership

```text
Earth blueprint (.vmesh)
├── Terrain and cloud geometry / recipes
└── Infrastructure part catalogue
    ├── Settlement recipe instances
    ├── Skyway recipe instances (two anchors)
    │   └── Optional A/B dispersal terminals
    ├── Hub recipe instances
    ├── Standalone dispersal terminals
    ├── Orbital elevators
    └── Tunnel joiners (inlet + two outlets)
```

`InfrastructurePart` is the CPU authoring record. `add_infrastructure()`,
`move_infrastructure()`, `rotate_infrastructure()`,
`move_infrastructure_endpoint()`, `edit_infrastructure()` and
`remove_infrastructure()` return an updated document without graphics dependencies.
`connect_infrastructure_endpoint(document, tunnel_id, end_b, {part_id, socket_id})`
attaches an end; an empty reference detaches it. Structures own their sockets;
tunnel recipes only reference them. There is no recursive ownership or tunnel-to-tunnel
dependency graph. `infrastructure_sockets()` describes the available named frames,
and `earth_connections.cpp` resolves/validates references and constructs routes.
`earth_infrastructure_edit.cpp` owns recipe persistence and scoped authoring;
`earth_infrastructure.cpp` owns geometry generation and replacement;
`earth_skyway.hpp` defines the CPU-only curve shared by its tunnel components.
`earth_structures.cpp` supplies terminal/elevator/joiner/support geometry, without knowing
about document identities, editor state or GPU resources. The parent builder
assigns ownership; all of it remains one ordinary instanced Earth mesh.
`earth_infrastructure_placement.hpp` shares the structure transform between hull
generation, socket frames and editor handles. Supports are generated in Earth
space after that transform, keeping their feet grounded. Height/property changes
regenerate only the selected part and its directly connected tunnels. Stable
topology uses sparse vertex uploads; toggling supports or crossing a spacing/count
threshold needs new topology. Terrain, clouds and unrelated parts are untouched.

The blueprint's `describe_blueprint_mesh()` declares part choices, placement
actions, `MeshPartChoice` dropdown edits and `MeshPartGizmo` modes, each containing one or more `MeshPartHandle`s.
`MeshPartConnection` declares eligible target parts and their `MeshPartSlot`s;
each slot supplies a marker and an attachment edit. The panel owns attachment
mode, and the viewport only routes target/socket clicks. `SurfaceMove::radial_range`
optionally adds an up/down axis through the existing translation tool, without
teaching the editor about Earth or tunnels. The CPU API
`place_infrastructure_endpoint(document, id, end_b, canonical_position)` sets both
longitude/latitude and height; `move_infrastructure_endpoint(..., lon_lat)` preserves height.
`place_infrastructure(document, id, canonical_handle_position)` likewise lifts
or moves a standalone structure, using the same declared handle anchor as the UI.
`BlueprintMeshPanel` adapts these to
UI/picking/background jobs; `EditingSession` remains the sole document/history
owner. The application only routes placement clicks and tool gestures; it does
not know about hub or tunnel types. A `MeshPartId` pairs an ownership-field name
with an integer, so cloud 1 and infrastructure part 1 are distinct identities.

`InfrastructureSettings` and `rebuild_infrastructure()` live in
`examples/support/earth_infrastructure.*`. Ownership is recorded in the ordinary
`earth/infrastructure` vertex field (0 other, 1 cities, 2 skyways, 3 hubs,
4 standalone terminals, 5 elevators, 6 joiners),
separate from cloud ownership. `earth/infrastructure-part` identifies the local
recipe instance; `earth/infrastructure/part/<id>` metadata stores its settings.
Recipe version 5 adds structure altitude, overall scale, scaffolding and tunnel
support spacing. Versions 1–4 remain readable with altitude 0, scale 1, scaffolding
enabled and automatic spacing. Versions 1–3 also default free-end altitude to zero;
versions 1/2 have no connections (v1 defaults terminal flags off). Version 6 adds
optional manual support parameters (up to 64, along the route from 0 to 1).
Versions 1–5 default to automatic placement. Version 7 adds tunnel size class;
versions 1–6 default to trunk. Version 8 adds optional Bézier controls and segment
count; versions 1–7 retain automatic paths. Version 9 adds the flared mouth's
inclination (0–60 degrees); older versions retain 30 degrees. Edits write version 9.
Existing catalogues are never silently populated with new parts.
This remains one ordinary instanced mesh draw
per blueprint; no runtime particles, textures, per-city objects or extra draw
calls. Metadata selects the illustrated night-emission DSL shading in
`examples/editor/mesh_shading.hpp`; the diagnostic path uses the same shading
and mesh faces. Rebuilds do not run the expensive cloud generator.

Reproduce the variant in a fresh output directory:

```sh
cmake --build build --target vng_make_earth
./build/vng_make_earth examples/assets NEW_DIRECTORY --future
```

To refresh an already edited connected-homeworld preset without resetting its
camera, animation, terrain, clouds or infrastructure settings, use
`--refresh-future` instead. It reads `earth_future.*` from the source directory
and writes a new copy into the output directory; neither mode overwrites files.
Existing imported blueprints acquire the new architecture when you use
**Apply infrastructure**, then **Apply to scene**.

## Savannah variant

`examples/assets/earth_savannah.vmesh` is a separate, importable Earth variant;
`earth_savannah.vscene` copies the existing Earth scene and its animation with
the same palette treatment. The original Earth files are unchanged. Vegetation
is modestly warmer/yellower, wet tropics retain more green, and existing dry
belts expand gently into scrub. Geometry, ocean colors, clouds and editing
metadata are retained from each source asset.

Open `earth_savannah.vscene` in the editor, or use **Import** to add the variant
mesh to another scene. Preview it with:

```sh
./build/vng_earth_demo examples/assets/earth_savannah.vscene
```

To reproduce copies into a fresh output directory (the tool refuses overwrite):

```sh
cmake --build build --target vng_make_earth
./build/vng_make_earth examples/assets NEW_DIRECTORY --savannah
```

## Original Earth

Earth is an **ordinary mesh blueprint**, not an editor-only drawing or a new
scene-object kind. It has smooth normals, simplified hand-drawn continents,
shallow terrain relief, cobalt oceans, broad green/desert/ice regions and raised
billowing cloud fronts/spirals. Its geometry is shared by all instances of the
blueprint. Dispersed cloud fringes add outline vertices without per-puff draw calls.

Detail is concentrated on coastlines, small islands and weather outlines. Land
samples use a 256×128 grid and clouds a 448×224 grid; only occupied patches emit
triangles. The smooth ocean stays at 128×64. Terrain/biome transitions are
continuous, avoiding the old triangle-sized color and height steps. The asset
and its editable draft both fit the existing editor scene budgets.

Dry regions include the Sahara/Sahel, Arabia, Central Asia, Taklamakan/Gobi,
northern Mexico, southern Africa and the Australian interior. Their irregular
boundaries blend through dry grass/scrub into warm sand. Finer, layered color
variation is sampled on the unit sphere and baked into vertex colors, not
loaded from bitmap textures. This keeps ordinary mesh importing, instancing and
diagnostic rendering unchanged; it is still an illustrated climate map, not a
scientific reconstruction.

The direction is illustrated, smooth low-poly forms with clear color groups and
restrained effects. Risk of Rain 2's broader environment-art treatment is a
reference, not a planet design to reproduce. No game assets, satellite photos or
downloaded models are used. Geography is deliberately approximate.

## Use it

```sh
./build/vng_earth_demo
./build/vng_editor_demo --scene examples/assets/earth.vscene
```

The scene starts on the Atlantic and turns slowly through a 90-second timeline.
Select a keyframe to edit its pose. **Play (in editor)** previews the turn.
Both the camera and rotation are normal editable scene values.

To add Earth to another scene, click **Import...** in the top toolbar and
choose `examples/assets/earth.vmesh`. It adds **EARTH / stylized homeworld** to
that scene's blueprint list and creates an instance. The blueprint's **+** creates
more instances; transforms remain per-instance. **Edit EARTH** opens normal mesh
editing, with the usual draft / Apply to scene boundary.

### Edit clouds

Open Earth's mesh view through **View** or its blueprint's **Edit** entry. The
**Blueprint geometry** tab includes **Earth clouds / mesh draft**:

- **Clouds visible** includes/removes the generated cloud layer.
- **Coverage** changes puff density; **Puff size** changes individual footprints.
- **Spiral size** expands/contracts the twisted weather systems independently.
- **Edge scatter** spreads puffs increasingly far from the main body of each
  cloud bank and toward the outer spiral arms, with smaller, separated wisps.
  The dense core stays cohesive. Range **0–2**; **0** disables the extra scattering,
  **1** is the new-generation default. The weather seed and heights do not change.
- **Altitude** moves clouds outward; **Height variation** controls billow relief.

Size, coverage and height values are multipliers (default 1).
Older saved Earth meshes start with Edge scatter **0** to match their baked
geometry; try **1** and **Rebuild clouds** to update them. Saved scenes are not
silently regenerated. Use mesh **Surface (4)** mode to see the cloud edges clearly.
Altitude starts at 1 to retain clearance above the terrain. All sliders also
have editable number fields. Fields stage locally; **Rebuild clouds** runs the
CPU recipe in the background and creates one undoable mesh-draft edit. No scene
keyframe selection is needed. The editor stays responsive while generating.

#### Move a whole cloud formation

After rebuilding a legacy mesh once, **Edit part** lists 14 named cloud banks
and 3 spirals. In **Surface (4)** mode, you can also **click any visible part of a
cloud formation** in the viewport. This selects the same entry and shows its
handle immediately; it does not rebuild geometry or create an undo step. Clicking
terrain or empty space clears the selection. Picking uses actual triangles and
respects occlusion, so a far-side formation cannot be selected through Earth.
Modes **1–3** keep their ordinary vertex/edge/face selection behavior.

Select a formation, set **Longitude (degrees)** / **Latitude (degrees)**
(sliders or number fields), then **Move cloud**. Selecting a visible formation
also shows a cyan **surface handle** in the viewport: **LMB-drag** its center to
move it along the sphere, **release** to confirm, or **Esc / RMB** to cancel.
The handle is hidden on the far side of Earth; orbit the camera to reach it.
**G** also starts surface movement; arrows move horizontally/vertically in the
camera view. **R** starts heading rotation, or drag the **Heading** ring. This
turns the formation around its local surface normal without lifting it off Earth.
For rotation, hold Left/Up to turn one way and Right/Down the other (60 degrees
per second at sensitivity 1; Shift is ten times finer). The gizmo's Sensitivity
control scales both mouse and arrow movement, with no keyboard-repeat delay.
Enter/LMB confirms keyboard gestures; Esc/RMB cancels.
The inspector also offers **Turn (degrees)** and **Rotate cloud**.

**Add cloud bank** and **Add spiral cloud** create and select a formation;
**Remove cloud** removes the selected one. These are blueprint-local identities,
not scene instances. Undo/redo, saved meshes and cloud rebuilds retain additions,
removals, placement and heading. The initial layout has 17 formations; edited
blueprints may have up to 128, subject to the mesh's vertex/face budgets.
Moving a formation carries it around the sphere, retaining its altitude, shape,
normals and hand-edited detail.
It does not move individual puffs or create scene instances. The names describe
the original locations; the coordinates show the current ones. **Whole blueprint**
returns to the coverage/size controls.

Movement is one undoable draft edit. Rebuilding clouds later preserves each
formation's placement and orientation, including while clouds are hidden.
Apply/Save/Export use the normal blueprint workflow below. Connecting faces or
edges between formations must be separated before moving one. Moves are explicit
via either **Move cloud** or the surface handle; they do not rebuild the particle mesh.

The handle follows the mouse immediately. Geometry catches up asynchronously,
with an **Updating blueprint...** spinner until the resulting worker image is
actually displayed (CPU completion alone does not clear it). A drag keeps one
running job and only the newest queued target; it does not accumulate old mouse
positions. The whole drag is one undo step. Cancellation restores the original
draft and rejects late job results. Switching back to **Whole blueprint** removes
the handle. Press **1–3** for mesh-component editing, or **5** for whole-mesh transforms.

Movement sends only that formation's changed positions, normals and placement
metadata to the preview worker. Undo/redo uses the same small patches; neither
path serializes the whole scene or reallocates the GPU mesh. Altitude/height-only
changes also reuse the cloud vertices and faces, updating positions and normals.
Coverage, puff/spiral size, scatter or visibility changes still regenerate cloud
geometry, but sample only each formation's footprint and replace only this
blueprint in the worker. Numeric/sliding fields remain staged until **Move cloud**
or **Rebuild clouds** is pressed; dragging the surface handle previews continuously.

Rebuild preserves ocean/land geometry, its custom attributes, and unrelated
metadata. Footprint regeneration **replaces hand edits to cloud geometry**;
height-only edits retain indexing and attributes but recompute cloud
heights/normals. Cloud/terrain connecting faces or edges cause a diagnostic
instead of being silently deleted. Newly
generated vertices receive zero values for unrelated custom attributes. Edits
made while rebuilding invalidate the old result rather than being overwritten.
Dense recipes still obey the editor's mesh/project budgets; a rejected build
leaves both the current draft and published geometry untouched. Scene/preview
documents now allow 64 MiB, and individual mesh sources 32 MiB, so ordinary Earth
cloud edits fit alongside their retained published version.

**Apply mesh to scene** publishes the draft to all instances of that blueprint.
**Save on disk** saves the project, including the draft and its settings, without
publishing. **Export new .vmesh** exports the inspected draft with its recipe
metadata. Legacy bundled Earth files also expose these controls; their generator
metadata identifies them, not the name “Earth.”

#### Transform the whole Earth

**Whole mesh (5)** targets the entire blueprint automatically, including hidden
geometry and clouds; no component selection is required. Use **R** to rotate,
**S** to scale, followed by **X/Y/Z** for an axis constraint.
**Ctrl+Left/Right** cycles Rotate, Scale and Free rotate (MMB drag). Move is
intentionally unavailable in this mode. Click or Enter confirms; Esc/RMB cancels.
The pivot is the mean vertex position, as with a complete component selection.
Each gesture is one undoable draft edit, and scene instances remain unchanged
until **Apply mesh to scene**.

Dragging updates a small render transform: no mesh rebuild, vertex upload or
full mesh serialization. Saving/exporting bakes positions and normals into a
disk copy without rewriting the live editing mesh. A saved authoring frame keeps cloud
handles, surface movement and later cloud rebuilds aligned after rotation or
nonuniform scaling. Choosing a cloud in **Edit part** leaves whole-mesh mode and
returns to Surface mode automatically.

```sh
./build/vng_earth_demo --time 45 --screenshot /tmp/earth-americas.png
./build/vng_earth_demo --analyze /tmp/earth-evidence
```

Capture paths must be new. Analysis includes the final composite, enhanced-shader
source-face IDs and mesh/instance provenance. Face-ID images isolate Earth;
they are not scene-wide occlusion masks.

## Code and ownership

```text
CPU blueprint authoring
├── support/earth_geography.hpp : longitude/latitude silhouettes
├── support/earth_assets.cpp   : coast clipping, biomes, relief, cloud geometry
│   └── CloudParticles        : seeded puff placement, spatial bins, merged density/height
├── support/earth_edit.cpp     : recipe + formation poses + cloud-only replacement/movement
└── scenes/earth_scene.cpp    : ordinary blueprint, instance, camera, timeline

Editor
└── BlueprintMeshPanel        : generic InspectorPanel + one background CPU job
    ├── describe_blueprint_mesh: blueprint-owned typed control declarations
    └── EditingSession        : revision-checked draft commit, undo, Apply, save

Runtime
├── MeshPrograms              : stable compiled program pair per lighting style
└── blueprint mesh renderers  : shared GPU meshes + instanced tickets
```

`example::earth::make_mesh()` returns a normal `vmesh::Document`. Loading and
rendering use saved geometry, not regeneration. The CPU-only `vng_earth_assets`
target also lets the editor explicitly rebuild clouds; it depends on content,
not on the editor, UI, renderer or OpenGL. `vng_editor_project` uses it for the
Earth control declaration; `vng_earth_scene` uses the project for scene authoring.
The surface uses existing `position`, `normal`, `color/0` and `emission` fields.
One blueprint still produces one native instanced draw for compatible tickets.

Metadata `render/lighting = "illustrated"` opts into the example runtime's
illustrated lighting convention. Its backend-neutral shader factory chooses
broad lighting bands, cool shadows, no large glossy highlight and a subtle cool
rim. Instanced and diagnostic paths call the same shading helper. The standard
path remains unchanged when the metadata is absent. This is a code-emission
choice, not a per-pixel branch or extra per-vertex storage. The `.vmesh` parser
does not interpret this metadata. Two relatives exist for other bodies:
`render/lighting = "matte"` is the standard model without its glint (bare
rock), and `"regolith"` is the illustrated night convention with the rim at a
quarter strength, since airless ground should not glow at grazing angles like
an atmosphere (the voyage's Moon uses it).

`CloudParticles` in `support/earth_clouds.hpp/.cpp` places differently sized,
weighted and elevated puffs along irregular fronts and broken spiral arms.
Edge scatter uses a separate seeded stream to drift puffs in the local tangent
plane, increasingly with distance from the front's curved spine/endcaps or the
spiral's center. A quadratic ramp preserves dense cores; outer puffs become
slightly smaller and more separated.
Spirals have an open eye surrounded by broken, curved clusters instead of solid
overlapping rings. Uneven gaps expose ocean inside the central body. Three unequal
rainbands widen their spacing, taper, and break up outward; mirrored winding,
seeded orientation, and slight stretching keep the storms from looking cloned.
The compact eye, uneven inner body, and diffuse outer bands use
[NASA's Hurricane Gordon observation](https://science.nasa.gov/earth/earth-observatory/hurricane-gordon-6940/)
as a visual reference, not as a texture. Fine cloud-top shading is baked into
vertex colors so the denser center does not become an unshaded white disk.
Each formation's overlapping kernels merge into its own density/height field,
with softened slope normals and density-dependent white/blue-gray coloring.
An `earth/cloud` UInt32 vertex field records ownership (0 = terrain, 1–17 =
formations); no generated triangle spans two formations. Recipe metadata retains
normalized rotations, so surface movement and later rebuilds agree. The generic
`BlueprintMeshPanel` only knows a part ID/name and an Inspector declaration, not
Earth coordinates. The mesh still uploads and renders in one instanced batch.
Puff footprints are 15% broader and spiral systems 25% larger than the initial
particle version. Billow relief is halved about its reference mid-height, then
the flattened layer's altitude above the unit-radius ocean is multiplied by 0.7.
The normal slopes use the same height scaling, and cloud bases stay above terrain.
This produces billows, patchy edges and gaps rather than uniform ribbons or
separate round beads. A temporary 3D grid limits sampling to nearby particles.
The particles and grid belong to the CPU builder and are not runtime scene
instances. Only the resulting opaque cloud geometry is saved: no bitmap cloud
textures, transparent particle sprites, sorting or per-puff draw calls.

Clouds are static parts of the mesh and rotate with it, **not a live particle
simulation or volumetric weather system**. The rim is stylized
surface lighting, not volumetric atmospheric scattering. There are no animated
weather maps, city lights or ground-level terrain details yet.

## Rebuild assets

```sh
cmake --build build --target vng_make_earth
mkdir /tmp/new-earth-assets
./build/vng_make_earth examples/assets /tmp/new-earth-assets
```

The generator refuses to overwrite either output, protecting editor edits.
It writes an importable `earth.vmesh` and a self-contained `earth.vscene`.

Tests cover deterministic generation, particle-bin/reference equivalence,
cloud height/normal variation, contour resolution, normals/winding,
continent/island/ocean placement, import, scene/draft persistence, shared instance ownership, GPU batching, lighting
changes, real rendered colors and diagnostic provenance.

`vng_editor_e2e_tests --earth` walks through the real mesh controls, background
rebuild, preview, Undo/Redo and Apply boundary, with screenshots.
