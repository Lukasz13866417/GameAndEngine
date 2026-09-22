# Earth / stylized homeworld

See [visual direction](art_direction.md) for the broader game-wide style target.

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
documents now allow 32 MiB, and individual mesh sources 16 MiB, so ordinary Earth
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
does not interpret this metadata.

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
