# Codebase: start from the component you need

The [interactive Codebase tree](explore/codebase.html#codebase) is the navigable
version of this tour. New readers can begin with the
[plain overview](explore/index.html) and a [concrete demo](explore/demo.html).
For actual classes rather than build targets, use the separate
[expandable C++ walkthrough](explore/walkthrough.html): it follows `Record`,
`Expr`, `Renderer`, `Frame`, `EditingSession`, `DocumentPatch` and `Runtime`,
with API excerpts and explicit ownership/borrowing on each card.
Open the HTML files locally: they work offline, including
search, source links and expandable dependency lists. This document explains
the important paths without requiring JavaScript.

The engine supplies reusable C++23 libraries. The editor is an application built
from them, with a UI process and a separate preview/rendering worker. OpenGL 4.6
and GLFW are the implemented graphics/window backends. Backend independence
means neutral contracts and explicit concrete implementations—not that another
graphics backend already exists.

## Three maps, not one misleading tree

- **Source location:** where a declaration, implementation or test lives.
- **Ownership:** who stores a value and determines its lifetime. This should be
  understandable as a tree, with explicit borrowed collaborators.
- **Dependencies:** what a component needs to compile/link or perform a call.
  Shared dependencies form a graph; they are not secretly owned children.

The interactive catalog names actual CMake targets and shows their direct
`target_link_libraries` entries. Its reverse **Used by** lists are derived from
those same entries. They cover mapped targets, not every example/test executable
or every C++ `#include`. Build options can disable conditional targets. See
[CMakeLists.txt](../CMakeLists.txt) for the authoritative configuration and
[the guide's tests](explore/tests/content.test.cjs) for drift checks.

## Where files live

| Location | Responsibility | Start here |
| --- | --- | --- |
| `include/vng/` | Public types, templates and API declarations | [gfx/record.hpp](../include/vng/gfx/record.hpp), [render/renderer.hpp](../include/vng/render/renderer.hpp) |
| `src/` | Compiled implementations | [shader/ir.cpp](../src/shader/ir.cpp), [opengl/commands.cpp](../src/opengl/commands.cpp) |
| `examples/*.cpp` | Runnable feature-focused programs | [file_mesh_direct.cpp](../examples/file_mesh_direct.cpp), [editor.cpp](../examples/editor.cpp) |
| `examples/support/` | Shared demo setup, effect policy and CPU asset generators; never includes the editor | [sun_shaders.cpp](../examples/support/sun_shaders.cpp), [window_loop.hpp](../examples/support/window_loop.hpp) |
| `examples/scenes/` | Authored scene generators and the shared scene demo, built on the editor document model | [scene_demo.cpp](../examples/scenes/scene_demo.cpp), [solar_system_scene.cpp](../examples/scenes/solar_system_scene.cpp) |
| `examples/editor/` | Editor document, authoring, interaction, communication and rendering policy | [editing_session.hpp](../examples/editor/editing_session.hpp), [app.cpp](../examples/editor/app.cpp) |
| `tests/` | Compile-fail, CPU, GPU, application and native interaction checks | [editor/editing_session_tests.cpp](../tests/editor/editing_session_tests.cpp), [editor/e2e_tests.cpp](../tests/editor/e2e_tests.cpp) |

Every directory under `include/vng/` and `src/` belongs to exactly one
target of the same name, so a header's path tells you what it links. `vng_gfx`
and `vng_render` are header-only. OpenGL integration headers live under
`include/vng/render_opengl`, `include/vng/glfw_opengl` and the other
`*_opengl` directories, never under the neutral `include/vng/render`.
`tests/layering/layering.test.cjs` checks this on every test run.

## Follow a mesh from bytes to pixels

Read [file_mesh_direct.cpp](../examples/file_mesh_direct.cpp) for the whole path
without a renderer wrapper. Its important steps are:

1. Define semantic tags and a `gfx::Record` in
   [file_mesh_types.hpp](../examples/file_mesh_types.hpp). A semantic identifies
   meaning; a codec defines stored bytes. A `VertexStream<Record>` owns CPU data.
2. Map file fields with `content::vmesh::schema<Vertex>()`, then `schema.map(...)`.
   The loader produces a CPU mesh. Parsing has no graphics-context requirement.
3. Build vertex/fragment stages with DSL expressions and link their contracts.
   The lambda executes once to record typed IR; it is not a per-frame callback.
4. Call `render::compile_program(device, shader_program)`. The neutral facade
   forwards through C++ argument-dependent lookup to the device's backend.
5. The [OpenGL compiler integration](../src/opengl/compile_program.cpp) invokes
   [GLSL emission](../src/glsl/emitter.cpp), compiles driver shaders and creates a
   backend program. CPU/GPU argument compatibility remains typed.
6. Upload the mesh, begin a frame, select graphics state, call
   `commands.run(program, model, brightness)`, supply the view and draw. `run`
   selects shaders/arguments; `draw` submits geometry. Presenting is separate.

The important build direction is:

```text
vng_shader → vng_gfx → vng_core
vng_glsl → vng_shader
vng_opengl → vng_gfx, vng_render, vng_glsl, vng_resources
vng_window_glfw → vng_core
vng_glfw_opengl → vng_opengl + vng_window_glfw
```

Here `A → B` means **A depends on B**, not “A owns B.” GLAD/GLFW adapter links
are listed separately in the interactive map. The OpenGL target does not depend
on GLFW; the bridge explicitly knows both backends.

For a concrete renderer, read
[FileMeshRenderer](../examples/file_mesh_renderer.hpp) and
[its implementation](../examples/file_mesh_renderer.cpp). It derives from
`opengl::Renderer<FileMeshDraw>`, creates its shaders internally, owns persistent CPU/GPU
resources and takes ticket spans. The base provides compile-time identity; it
does not impose one technique, virtual dispatch, or a fixed draw-state policy.

## The APIs behind that path

### CPU records: meaning and storage are separate

This is the essential contract from
[file_mesh_types.hpp](../examples/file_mesh_types.hpp):

```cpp
struct Position : vng::gfx::Semantic<vng::Vec3> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};
using Vertex = vng::gfx::Record<
    Position,
    vng::gfx::as<Color, vng::gfx::unorm8x4>>;
```

`Position` and `Color` are identities, not field indices. A `Color` still means
`Vec4` when its four components occupy normalized bytes in this particular
record. `Record` explicitly calculates field offsets and stride; it does not
depend on tuple/inheritance layout. A [VertexStream](../include/vng/gfx/vertex_stream.hpp)
owns contiguous CPU records:

```cpp
vng::gfx::VertexStream<Vertex> vertices(3);
vertices[0].set(Position{}, {-0.5F, -0.5F, 0.0F});
vertices[0].set(Color{}, {1.0F, 0.0F, 0.0F, 1.0F});
auto upload_bytes = vertices.bytes(); // borrowed span, no upload yet
```

`get` decodes the logical value; `set` encodes it. Resizing can invalidate record
references and byte spans. A CPU [Mesh](../include/vng/gfx/mesh.hpp) adds faces and
optional edges to one or several typed streams. It is not a scene instance or
a GPU allocation. Different physical layouts can satisfy the same shader's
semantic input contract. GPU mesh preparation resolves and caches the needed
vertex-input binding internally; callers normally do not write attribute tables.

### Shader construction and shader execution happen at different times

From [file_mesh_renderer.cpp](../examples/file_mesh_renderer.cpp):

```cpp
auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
    "file_mesh_fragment",
    [](auto& stage, vng::dsl::Float brightness) {
        const auto source = stage.input(Color{});
        const auto color = vng::dsl::vec4(source.xyz() * brightness, source.w());
        stage.observe(SurfaceColor{}, color);
        return stage.output(field<vng::shader::Color<0>>(color));
    });
```

The lambda runs once while constructing the stage. `Float` is `Expr<f32>`, a
builder pointer plus value ID; it is not a changing CPU float. Operations append
typed IR; copying an expression just copies the handle. A complete output record
is required, and linking validates stage interfaces before backend compilation.
Do not keep expression handles after their building context is gone.

The `brightness` lambda parameter becomes a **live program argument**. Later,
`commands.run(program, brightness)` accepts an ordinary CPU `f32` and uploads its
current value. The direct demo additionally has a matrix argument in its vertex
stage, so its linked call is `run(program, model, brightness)`. Capturing a C++
variable is not this mechanism: ordinary shader-building captures are not live
bindings. Compile-time literal operands prevent accidental runtime values from
silently becoming constants; explicit stage constants express snapshot intent.

[Arguments/ArgumentPack](../include/vng/shader/arguments.hpp) preserve exact CPU
types and flatten logical values: vector components, matrix columns, decoded
record fields. Their word views borrow the pack. The packed vertex bytes above
are **not** copied as though they were a uniform ABI. GLSL's
[parameter metadata](../include/vng/glsl/emitter.hpp) chooses the final uniforms.

[ModuleIR](../include/vng/shader/ir.hpp) stores tables of types, values, operations
and regions. Operations contain operands, results, nested-region references,
effect classification and source origin. This preserves shared producers and
ordered computation without binding the DSL to GLSL strings. `dump_ir` inspects
it before a graphics context exists; generated source and source maps expose
the next transition. Driver compilation is a later OpenGL operation.

### A renderer has a ticket type and a backend identity

The neutral base is now `render::Renderer<Ticket, Backend>` with no default
backend. A concrete OpenGL subclass uses the short
[opengl::Renderer](../include/vng/opengl/renderer.hpp) alias. A portable *algorithm*
can instead be a template; an outline is:

```cpp
template<vng::render::Backend B>
class ModelRenderer : public vng::render::Renderer<ModelDraw, B> {
public:
    // Result and ModelDraw are the application's types in this sketch.
    Result render(typename B::frame_type& frame,
                  const vng::render::RenderView& view,
                  std::span<const ModelDraw> draws);
};
```

[`Backend`](../include/vng/render/backend.hpp) names `device_type` and
`frame_type`. `backend_t<T>` reads a bound type's backend identity;
`SameBackend<A, B>` compares them. The base is empty, nonvirtual and protected
against plain instantiation. It neither owns a device nor intercepts a direct
`renderer.render(...)` call. `RendererFor` and `render_one` check the real frame,
matching backend and callable batch signature. A compatible backend type is not
proof that two resources belong to the same device/context.

Factories use concrete types and C++ argument-dependent lookup to reach the
implementation. This is not a service registry or dynamic router. To support
another backend, implement its entry points and resource realization; naming a
new backend does not make raw OpenGL code portable. See the detailed
[renderer API guide](renderer_api.md).

### Programs, live state and frame lifetimes

[Commands](../include/vng/opengl/commands.hpp) separates the following jobs:

| Operation | Meaning |
| --- | --- |
| `graphics_state().set(setting)` | Change supported live state for subsequent draws. Other settings remain unchanged. |
| `run(program, args...)` | Select a typed program and the current argument values. Does not draw geometry or install a persistent graphics preset. |
| `bind(...)` | More granular explicit program binding. |
| `view(view)` | Supply the camera/projection information needed by this view. |
| `draw(mesh, ...)` | Submit geometry, optionally with per-instance data. |
| Window `present()` | Present after rendering; separate from program execution. |

[GraphicsState](../include/vng/render/graphics_state.hpp) is a static facade over
a backend state access object. `DepthTest`, `DepthWrite`, culling and blend modes
are neutral setting types. Backend extensions stay in their own namespace;
unsupported settings fail the type constraint. No persistent preset must be
allocated just to turn depth off for one draw.

A [Frame](../include/vng/opengl/frame.hpp) owns active frame state; Commands
borrows it. Copies share binding/state bookkeeping, not independent command
buffers. End the frame before resizing/reloading resources it borrows. The
[Device](../include/vng/opengl/device.hpp) validates context/thread ownership;
GPU resource creation, use **and destruction** require the compatible current
context. Keep window/context alive until all dependent resources are destroyed.
The OpenGL library does not create or depend on a GLFW window: the explicit
[bridge](../include/vng/glfw_opengl/glfw_opengl.hpp) provides that pairing.

### Providers recreate; owners replace

A [provider](../include/vng/resources/provider.hpp) retains how to produce a
resource. The owner retains the actual resource and decides when replacement is
safe. `Provided<T, P>` packages a ready resource with its recipe without creating
it again. `Shared<T>` is a read-only snapshot; swapping one owner's handle does
not silently rewrite all existing borrowers.

From [glow.cpp](../examples/glow.cpp):

```cpp
auto ring_builder = render::mesh_renderer_builder(device);
ring_builder.mesh(providers::mesh(scene::ring()));
auto rings = ring_builder.build();
if (!rings) return fail(rings.error());
```

The ready [MeshRenderer](../include/vng/resources_opengl/mesh_renderer.hpp) owns resources
and retained recipes. Its `update(device)` transaction stages replacements,
validates and commits a coherent bundle. It checks owner generation so a stale
transaction cannot overwrite a newer one. Reload helpers call retained providers;
failure should not install a half-valid replacement. This implementation is an
optional concrete renderer, not a mandatory ownership pattern imposed by the
empty renderer base.

For glow, an HDR target holds light values above one, bloom extracts/blurs them,
and compositing/tone mapping produces display color. Size-dependent resources
are resized between frames. This sequence is explicit rather than a hidden render
graph; [the bloom implementation](../include/vng/bloom_opengl/bloom.hpp) does not own
sun-specific pattern/displacement policy.

### Text, UI and rigging keep CPU meaning separate from rendering

[Font](../include/vng/text/font.hpp) shapes UTF-8 into glyph runs and rasterizes
glyphs. The text renderer owns atlas resources and batches. Font shaping is not
a full document-layout system with automatic fallback, paragraph bidi and
wrapping. A glyph index is not the same thing as a Unicode code point.

[Screen](../include/vng/ui/ui.hpp) owns retained widgets; panel handles refer to
them. Update input/layout first, then poll `clicked()`, `isPressed()`,
`changedText()` or `submittedText()`. Copy borrowed text views if retaining them.
The neutral DrawList is consumed by the UI renderer. Focus and hover are UI state;
authoring a scene change requires an explicit session operation. See
[the runnable UI example](../examples/ui.cpp).

[Armature](../include/vng/rig/armature.hpp) owns rest hierarchy, Pose stores
changing local bone transforms, and [SkinBinding](../include/vng/rig/skin_binding.hpp)
associates a mesh snapshot with one armature and its influences. Weights do not
own bone positions. GPU skinning realizes that binding and updates palettes from
poses; it does not rebuild geometry for every bone motion. The dependency is
`rig_opengl → rig`, never the reverse. Follow [rigging.cpp](../examples/rigging.cpp)
for two independent poses of one binding.

## Editor ownership

The application is composed of ordinary local objects in
[app.cpp](../examples/editor/app.cpp); the groups below are not extra manager
classes that users must instantiate.

```text
UI process · editor_example::run
├── EditingSession
│   ├── State
│   │   ├── Document: authored content, drafts, instances, camera, timeline
│   │   └── ViewportState: private camera, inspection/playhead state, sequence
│   ├── Active gesture + undo/redo + saved-content identity
│   ├── EditClipboard
│   └── SceneFile
├── Screens, panels and local multi-selection sets
├── ViewportInteraction → tools (borrows EditingSession&)
├── PreviewSession → worker/build processes and communication handles
├── PreviewUpdates + PreviewMailbox → delivery/pending-image state
└── Display resources + optional detached editable viewport window

Worker process · editor_worker.cpp
├── WorkerEndpoint
├── Worker-side State and playback/view-request handling
├── Window/context + Device
└── Runtime
    ├── Shared mesh program
    ├── Blueprint → MeshResource map
    │   └── BlueprintMeshRenderer
    │       ├── GPU mesh
    │       ├── Reusable solid/wireframe instance batches
    │       └── Borrowed shared Program (not owned here)
    └── Sun, HDR targets, softening, bloom and presentation resources
```

`EditingSession::state()` exposes const model access; typed methods perform
authored edits. `viewport()` deliberately allows private view changes separately.
It publishes `EditNotice` values and does not depend on UI widgets, IPC or a GPU.
See [editing_session.hpp](../examples/editor/editing_session.hpp) and the
[session API guide](editing_session.md).

A `Document` keeps applied mesh geometry separate from `mesh_drafts`. Scene
instances resolve applied geometry. Mesh inspection can show a draft; **Apply**
publishes it in memory, while saving persists the two representations without
implicitly applying. Blueprints survive instance deletion. Instance transforms
and region boundaries belong to instances, not shared mesh geometry.

The worker and UI do not share mutable `State` objects or OpenGL handles.
The worker's context-owning loop samples playback, executes effect callbacks and
renders. There is no separate simulation thread hidden in this arrangement.
`Runtime` borrows render inputs; it owns GPU realization, not the authoritative
document. Its per-blueprint mesh renderer explicitly borrows a shared program
that outlives it. See [runtime.hpp](../examples/editor/runtime.hpp) and
[blueprint_mesh_renderer.hpp](../examples/editor/blueprint_mesh_renderer.hpp).

## Follow one interaction

An authored move follows this path:

```text
Tool/panel → EditingSession operation → EditNotice / DocumentChanges
  → PreviewUpdates → DocumentPatch or structural snapshot
  → worker validation + acknowledgment + targeted GPU invalidation
  → rendered frame + camera/revision metadata → PreviewMailbox → display
```

[PreviewUpdates](../examples/editor/preview_updates.hpp) coalesces affected
properties/vertices while one authored revision waits for acknowledgment. It
does not accumulate a historical copy for each mouse move. Structural changes,
reconnects and recovery can require full snapshots; a narrow patch is not always
possible.

Private camera movement and selection instead use an absolute
[ViewportRequest](../examples/editor/viewport_session.hpp), sent on the
replaceable `send_latest` lane. This does not advance the document revision.
Never use that lane for authored deltas that must all be preserved.

[Presented frame metadata](../examples/editor/presented_view.hpp) describes the
image actually displayed, not the most recent request. Overlays/picking must use
the right image/camera pair. Full multi-selection sets stay in UI tools/panels;
the worker receives the active inspection target rather than every selection
as a document mutation.

The sidebar's **Scene**, **Keyframe values**, and **Instance properties** tabs
share the right-hand area. Placement belongs to
[editor_layout.hpp](../examples/editor/editor_layout.hpp) and `app.cpp`.
The editable pop-out remains in the UI process with the same session. Independent
Play is a worker-owned window and can exist separately.

## Choose the narrowest implementation boundary

| Change | Start in | Useful checks |
| --- | --- | --- |
| Shader operation or argument typing | `include/vng/shader/dsl`, `include/vng/shader`, `src/shader` | `tests/shader`, `tests/compile_fail`, `tests/glsl` |
| Draw policy / live graphics state | Concrete renderer and `opengl/commands.hpp` | `tests/render`, `tests/opengl` |
| Resource reconstruction / transactional reload | Provider recipe and concrete resource owner | `tests/resources`, `tests/opengl/resource_owner_tests.cpp` |
| Authoring operation / undo / clipboard | `EditingSession`, `DocumentChanges`, `DocumentPatch` | `tests/editor/editing_session_tests.cpp`, operation-specific tests |
| Gizmo, region points or component transform | `ViewportInteraction`, the tool, `ComponentTransform` | `tests/editor/component_transform_tests.cpp`, `region_ui_tests.cpp` |
| Picking speed | `EditableMesh`, `TriangleBvh`, editor selection/projection | `tests/editor/picking_acceleration_tests.cpp` |
| Widget behavior / text interaction | `src/ui/ui.cpp`; renderer only for visual realization | `tests/ui/ui_tests.cpp`, `tests/opengl/ui_renderer_tests.cpp` |
| Scene rendering/batching | `Runtime`, `BlueprintMeshRenderer` | `tests/editor/mesh_batch_tests.cpp`, `runtime_tests.cpp` |
| End-to-end editor behavior | App/panel/tool and `Automation` observation/input surface | `tests/editor/e2e_tests.cpp` |

These are starting points, not permission to move application policy into the
engine. Likewise, do not invent a new abstraction just to make the diagram tidy.
See [editor boundaries](editor_boundaries.md) for timing/caching detail and the
[interactive component catalog](explore/codebase.html#code-components) for exact
direct dependencies, API entry headers, implementations and test links.

## Limits of this map

The catalog covers the principal reusable targets and the editor's composition.
It does not inventory every demo executable, every test or every individual
header include. Ownership descriptions are manually verified against members
and composition sites; CMake checks cannot prove runtime lifetime correctness.
The [maintenance note](explore/maintenance.md) records the inspected source
landmarks, checks and next useful documentation work.
