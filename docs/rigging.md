# Character rigging

Rigging is built around independent mesh and armature assets. A `SkinBinding`
connects exactly one immutable mesh snapshot to one immutable armature, while
each character instance has its own mutable `Pose`.

There is no ML integration, asset database, animation graph, or imported skeleton
format in this implementation.

## Small public API

The CPU API is available through `<vng/rig/rig.hpp>`. The normal workflow is:

```cpp
namespace rig = vng::rig;
namespace render = vng::render;

rig::ArmatureBuilder builder;
auto upper_arm = builder.add_bone("upper_arm");
auto forearm = builder.add_bone(
    "forearm", upper_arm,
    rig::Transform{.translation = {0, 1, 0}});
auto armature = builder.build();

auto skin = rig::bind(mesh, *armature);
skin->set_weights(0, {{upper_arm, 1.0F}});
skin->set_weights(1, {{upper_arm, 0.25F}, {forearm, 0.75F}});
// Supply weights for every remaining vertex before making a renderer.

auto pose = armature->rest_pose();
auto bend = rig::rotation({0, 0, 1}, vng::degrees(35));
pose.set_local_rotation(forearm, *bend);

auto renderer = render::make_skinned_mesh_renderer(device, *skin);
auto result = renderer->render(frame, view, render::SkinnedDraw{
    .pose = pose,
    .transform = {.translation = {2, 0, 0}},
});
```

Error checks are omitted above for readability. Asset factories and mutations
return `std::expected<..., rig::Diagnostic>`; backend renderer factories and
draws return `std::expected<..., opengl::Diagnostic>`.
`add_bone()` is the authoring exception: it returns a bone handle and records
errors for `build()`, avoiding a check after every hierarchy declaration.

The renderer factory is declared in `<vng/rig/skinned_mesh_renderer.hpp>`;
include `<vng/rig_opengl/skinned_mesh_renderer.hpp>` to enable the OpenGL backend.
The device determines the backend. The caller never writes GLSL, resolves
attribute locations, creates a VAO, packs a matrix buffer, or supplies shaders.

## Responsibilities and lifetime

| Type | Owns or refers to | Changes during ordinary animation? |
| --- | --- | --- |
| `gfx::Mesh<Record...>` | Editable geometry and topology | No |
| `rig::Armature` | Shared immutable hierarchy and rest transforms | No |
| `rig::SkinBinding<Mesh>` | Immutable mesh snapshot, armature, authored influences, bind relationship | No |
| `rig::Pose` | Armature handle and per-bone local transforms | Yes |
| `opengl::SkinnedMeshRenderer` | Copied binding data, uploaded geometry/influences, shader products, palette buffer | Palette only |
| `render::SkinnedDraw` | Borrowed pose, value-owned placement and raster choices | Per submission |

`bind(mesh, armature)` copies an lvalue mesh or moves an rvalue into an immutable
owning snapshot. Later edits, resize operations, or vertex reordering on the
original mesh do not change what the binding means. Creating a renderer takes
another snapshot of the authored weights; editing a binding afterward requires
a new renderer upload to display those changes.

Armature copies share the same identity and data. Bone handles include their
owning armature identity: a bone or pose from an unrelated armature is rejected
even if its name and array index happen to match.

`SkinnedDraw::pose` is a `std::reference_wrapper<const Pose>` and rendering is
synchronous. Keep the pose alive through `render()` or `capture()`; tickets do
not copy, own, or queue it. Several tickets can borrow the same pose, or use
different poses with one renderer.

OpenGL resource lifetime follows the rest of the engine: the owning compatible
context must be current during creation, rendering, and destruction. Declare
the application/window before its renderers so resources are destroyed first.

## Armatures, transforms, and poses

Add parents before children. This makes cycles and unresolved parents
unrepresentable in valid builders and yields parent-first dense arrays. Bone
names are nonempty and unique; store `BoneId` handles for animation rather than
looking up names every frame. `armature.bone("forearm")` is available for setup.

`ArmatureBuilder::add_bone()` records any construction error; `build()` reports
it as a diagnostic. Successfully built armatures are immutable. The builder
cannot append more bones afterward.

`rig::Transform` contains:

```cpp
Vec3 translation{};
Quat rotation{};       // identity by default; must be a unit quaternion
float scale{1.0F};     // positive uniform scale
```

Nonuniform scale, negative scale, and arbitrary shear are intentionally not part
of this initial contract. `rig::rotation(axis, angle)` constructs a validated
quaternion; angles use the existing `degrees()` / `radians()` helpers.

`pose.set_local(bone, transform)` replaces a parent-relative transform.
`set_local_rotation()` replaces only its rotation, preserving translation and
scale. Both are absolute values, not deltas from rest. `pose.reset()` restores
the rest pose. `pose.local()`, `pose.globals()`, and `pose.revision()` support
inspection and custom animation code.

There is no animation player yet. IK, constraints, keyframe sampling, and future
animation blending can all produce a `Pose` without changing the binding or
renderer contract.

## Binding and weights

By default the mesh and armature use the same local coordinate system. An
explicit authored relationship is supported:

```cpp
auto skin = rig::bind(mesh, armature, rig::BindOptions{
    .mesh_to_armature = {.translation = {0, -1, 0}, .scale = 0.01F},
});
```

For column vectors, a bone's mesh-local deformation is:

```text
inverse(mesh_to_armature)
    * posed_global[bone]
    * inverse(rest_global[bone])
    * mesh_to_armature
```

The bind/rest pose therefore reproduces the undeformed mesh. Object placement
is applied after this deformation, exactly once; camera projection follows it.

Weights refer to vertices of the captured mesh, not position coordinates. Two
coincident vertices remain two vertices. A future editing/remeshing operation
must explicitly transfer weights into a newly bound mesh; vertex-count equality
is not a sufficient compatibility check.

Authoring permits any number of influences per vertex. Every bone must belong
to the binding's armature; weights must be finite and nonnegative; duplicate
bones are rejected. Empty/unweighted vertices and sums different from one are
rejected by final validation and upload.

Useful authoring operations are:

```cpp
skin->validate();
skin->normalize_weights();
auto report = skin->prune_weights(4);
```

Normalization and pruning are explicit. Pruning retains the largest positive
influences, renormalizes them, and reports affected vertices and discarded
weight. Upload never silently drops influences.

The default renderer supports four positive influences per vertex. Select eight
without changing the editable record or authored asset:

```cpp
auto renderer = render::make_skinned_mesh_renderer(
    device, *skin,
    render::SkinnedRendererOptions{.max_influences = 8});
```

The derived GPU influence stream uses 32 bytes per vertex for four influences
or 64 bytes for eight. The four-influence shader has no second index/weight
attribute set; this choice changes the actual storage and input contract.

For lower-level users, `skin->weights()` exposes backend-independent
`SkinWeights`, and `pack(4)` / `pack(8)` return neutral dense runtime indices and
weights. `palette(pose)`, `deform_points(points, pose)`, and
`deform_normals(normals, pose)` provide CPU reference calculations. These do not
depend on a window, graphics context, shader compiler, or native API.

## Geometry semantics

The default factory recognizes the provided convenience semantics:

```cpp
using Vertex = vng::gfx::Record<
    vng::gfx::Position, // Vec3, required
    vng::gfx::Normal,   // Vec3, optional
    vng::gfx::as<vng::gfx::Color, vng::gfx::unorm8x4> // Vec4, optional
>;
```

Missing color means white; missing normals means unlit rendering. Existing
custom semantic identities can be mapped once when creating the renderer:

```cpp
auto renderer = render::make_skinned_mesh_renderer(
    device, *skin,
    render::GeometryFields{
        .position = MyPosition{},
        .normal = MyNormal{},
        .color = MyColor{},
    });
```

Roles are type-checked; field order, separate vertex streams, and physical
codecs remain independent of their meaning. The renderer builds its own runtime
vertex representation once. Skin indices and weights are additional derived
data, not new requirements on the user's mesh record.

Normals use weighted per-bone inverse-transpose transforms, normalization, and
the placement normal transform. This is conventional linear-blend normal
skinning, not a guarantee of exact normals for every possible deformation.
Finite nonzero authored normals are normalized once at upload. If opposing
influences cancel a normal completely, GPU output uses a zero normal and ambient
lighting rather than NaNs; the CPU normal reference reports a degenerate-normal
diagnostic. For nonzero blended normals, the shader rescales their magnitude
before normalization to avoid scale-dependent overflow or loss of lighting.
Basic lighting can be disabled explicitly with `.lighting = false`.

## Rendering and diagnostics

`SkinnedMeshRenderer` derives from `opengl::Renderer<SkinnedDraw>` and exposes the usual
batch entry point:

```cpp
renderer.render(frame, view, std::span<const render::SkinnedDraw>{draws});
```

`view` must contain a camera snapshot and match the frame extent. Each ticket
can choose placement, `depth_test`, `depth_write`, and `cull`. The renderer owns
its policy and establishes these choices through the existing graphics-state
API. `stats()` reports draw calls, palette uploads, and submitted triangles.

Geometry and weights are uploaded once. Animation updates the bone
matrix palette, not every vertex. Object placement and its normal matrix are
ordinary typed shader arguments, separate from that genuinely indexed palette.
Moving an object without changing its pose therefore does not upload the bone
buffer again. Submission recomputes and compares actual
palette contents; identical contents skip the GPU upload, without relying on
pose addresses or revisions as lifetime-sensitive cache keys. The shader reads that palette through a
backend-neutral indexed matrix resource in the DSL. OpenGL lowers it to a
read-only storage buffer; native indexed buffer range and generic binding state
are preserved around renderer use.

`source()` exposes the emitted production GLSL and interface metadata.
Enhanced capture uses the same vertex shader deformation and palette:

```cpp
auto evidence = renderer.capture(
    frame, view,
    render::SkinnedDraw{.pose = pose},
    vng::analysis::CaptureRequest::standard());
```

For deformed world-space values, request the renderer's named observations:

```cpp
auto request = vng::analysis::CaptureRequest::standard()
    .observe(render::SkinnedWorldPosition{})
    .observe(render::SkinnedWorldNormal{});
auto evidence = renderer.capture(frame, view, draw, request);
auto positions = evidence->observation<render::SkinnedWorldPosition>();
```

`SkinnedWorldPosition` is always available. `SkinnedWorldNormal` is available
only when the source mesh contains the mapped normal semantic, whether or not
lighting is enabled. Requesting it for a mesh without authored normals returns
a diagnostic rather than reporting the renderer's internal fallback direction
as surface data.

Capture currently accepts one ticket with the default depth/culling choices
and a linear-output frame. It returns ordinary `FrameEvidence`: rendered color,
device depth, surface identity, source topology, and shader artifacts. It is
an explicit diagnostic operation with readback, not an implicit per-frame cost.
Capture workload identity includes the finalized geometry, influences and actual
submitted matrices, so different poses cannot be mistaken for the same workload.
After capture, draw statistics count the canonical pass plus requested observation
passes; after normal rendering they count the submitted tickets. Repeated identical
palettes skip buffer writes, but independent poses are compared by matrix values,
not potentially equal revision numbers.

The supplied renderer issues one draw per ticket; it does not yet combine
different characters into one hardware-instanced draw. Reusing the same dynamic
matrix buffer is correct but may stall under heavy workloads; ring-buffered
palette uploads are a future performance improvement.

For custom renderers, `<vng/shader/skinning.hpp>` supplies
`dsl::blend_matrices<Binding>(stage, joints, weights, offset, stride)`. It is an
ordinary DSL helper over `stage.matrix_buffer<Binding>(UInt)`, not a rig-specific
IR operation. Each group blends four matrices; the eight-influence path adds
two groups. Callers must supply valid indices even for zero-weight slots, and
prevent overflow in the offset/stride calculation. The built-in renderer handles
both automatically.

## Dependency tree

Arrows below mean “depends on”; aliases correspond to CMake targets.

```text
vng::rig
    -> vng::gfx
        -> vng::core

vng::rig_opengl
    -> vng::rig
    -> vng::render_opengl
        -> vng::render -> vng::gfx
        -> vng::analysis -> vng::gfx
        -> vng::glsl -> vng::shader -> vng::gfx
        -> vng::opengl
            -> vng::gfx + vng::render + vng::glsl (shared branches)
            -> GLAD (private native procedure loader)
    -> GLAD (private native palette binding)

demo / OpenGL tests
    -> vng::rig_opengl
    -> vng::glfw_opengl
        -> vng::window_glfw -> GLFW (private window dependency)
        -> vng::opengl
```

There are no new third-party dependencies for rigging. In particular,
`vng::rig` does not depend on OpenGL, GLFW, the DSL, text/font libraries, or
Python. The DSL's read-only matrix resource is general shader infrastructure;
it does not depend on rigging types. GLFW remains an application/test integration
choice, not a dependency of the character renderer itself.

The initial implementation deliberately stops short of persistence/import,
editable control rigs, IK, animation clips, automatic weight generation,
dual-quaternion skinning, blend shapes, and crowd-instance batching.
