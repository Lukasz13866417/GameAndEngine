# Renderers, frames, and backend commands

The ordinary API now uses `compile_program`, `context.run(program)`, and
`context.graphics_state().set(setting)`. See [programs and graphics state](graphics_state.md)
for current examples, backend compatibility, defaults, and lifetime rules.

A renderer is application policy. Its ticket describes one submission; the
concrete renderer owns persistent resources and decides how to order work,
which program to bind, and when fixed-function state changes.

```cpp
struct CharacterDraw {
    CharacterId character;
    Mat4 model;
    MaterialId material;
    bool selected{};
};

class CharacterRenderer final
    : public vng::opengl::Renderer<CharacterDraw> {
public:
    std::expected<void, vng::opengl::Diagnostic> render(
        vng::opengl::Frame& frame,
        const vng::render::RenderView& view,
        std::span<const CharacterDraw> draws);

private:
    // Programs, GPU meshes, staging storage, caches, ...
};
```

Include `<vng/opengl/renderer.hpp>` for this explicitly OpenGL-only policy.
`opengl::Renderer<Ticket>` is an alias for
`render::Renderer<Ticket, opengl::Backend>`, not a wrapper or another owner.
The neutral base is empty, zero-state and non-virtual. It supplies
`ticket_type` and `backend_type`, giving orchestration an explicit ticket/backend contract.
It does not own an implementation, forward calls, select a backend, or impose
one shader on the derived class. Its protected construction and
destruction also prevent accidentally instantiating a meaningless plain
renderer.

`RendererType<T>` recognizes a concrete public derivation, while
`RendererFor<T, Frame>` checks that the frame is valid, both declare the same
backend, and the canonical batch call exists. Even an unconstrained templated
`render()` cannot pass this check for another backend just by accepting any type.
These checks are compile-time only; there is no virtual
dispatch, type erasure, per-ticket allocation, or CRTP syntax. `render_one` is
the optional one-ticket convenience:

```cpp
static_assert(vng::render::RendererFor<
    CharacterRenderer,
    vng::opengl::Frame>);

auto result = vng::render::render_one(
    renderer, frame, view, CharacterDraw{/* ... */});
```

The batch overload remains the renderer's fundamental API because it leaves
room for sorting and coalescing work before drawing.

## Backend identity and portable policies

`render::Renderer<Ticket, Backend>` requires both template arguments. There is
no default "backend-independent" marker. A renderer implementation can be
portable; its instantiated resources belong to one concrete backend.
The following declaration illustrates that distinction (its method bodies and
resource choices remain the application's responsibility):

```cpp
template<vng::render::Backend B>
class CharacterRenderer final
    : public vng::render::Renderer<CharacterDraw, B> {
public:
    static auto create(typename B::device_type& device, CharacterAssets assets);
    auto render(typename B::frame_type& frame,
                const vng::render::RenderView& view,
                std::span<const CharacterDraw> draws);
};

// A factory can infer the backend from the device; callers need not name it.
template<vng::render::BackendBound Device>
auto make_character_renderer(Device& device, CharacterAssets assets) {
    using B = vng::render::backend_t<Device>;
    return CharacterRenderer<B>::create(device, std::move(assets));
}
```

`render::Backend` requires a backend identity type with `device_type` and
`frame_type` class aliases. `opengl::Backend` only names forward-declared
OpenGL types; it has no runtime state or service registry. `Device`, `Frame`,
`Commands`, and concrete renderer classes expose `backend_type`.
`render::backend_t<T>` reads that identity (including through cv/ref qualifiers),
and `render::SameBackend<A, B>` compares it.

An OpenGL specialization can use native commands and own OpenGL resources.
A portable algorithm must actually use operations offered by each intended
backend; using the shader DSL alone does not make its surrounding C++ portable.
The current production backend is still OpenGL, not a newly implemented Vulkan
or software backend. The neutral contract tests exercise two small test backends.

Matching backend types is not matching devices or live contexts. Two unrelated
OpenGL devices have the same backend identity; existing runtime ownership,
current-context and frame-lifetime checks still apply. Direct concrete method
calls follow their C++ signatures; the base does not intercept or forward them.

## Ownership and construction

The concrete renderer chooses its constructor or fallible factory. Backend
selection is therefore explicit where persistent backend resources are made:

```cpp
auto renderer = CharacterRenderer::create(
    device, shader_program, character_assets);
```

For an OpenGL renderer, creation can compile programs and upload
meshes. A renderer for another backend owns that backend's corresponding
objects. Nothing requires every renderer to use the same factory shape.

This also permits different levels of specialization:

- a conventional renderer can own neutral descriptions and realize them for
  its selected backend during `create`;
- an OpenGL-specific renderer can use OpenGL frame commands and backend
  resources directly;
- a backend-parameterized renderer can share policy between implementations;
  one concrete renderer may accept multiple frame variants of its declared backend.

Tickets should stay cheap and describe changing submission data. Shader
programs, GPU buffers, caches, and other persistent objects belong
to the renderer or to referenced asset owners, not in every ticket.

## Frame and view

`FrameDesc` describes target work: extent, target color encoding, and optional
clears. `RenderView` is an immutable camera snapshot for the same extent.

```cpp
auto view = vng::render::RenderView::create(camera, extent);
auto frame = vng::render::begin_frame(
    device,
    vng::render::FrameDesc{
        .extent = extent,
        .color_encoding = vng::render::ColorEncoding::srgb,
        .clear_color = std::array{0.02F, 0.03F, 0.05F, 1.0F},
        .clear_depth = 1.0F,
    });

renderer.render(*frame, *view, draws);
frame->end();
window.present();
```

Separating the target from the view matters for future off-screen targets,
multiple editor views, and pass scheduling. Camera-free work uses
`RenderView::without_camera(extent)`.

For the default window framebuffer, the frame's color encoding must match the
physical attachment encoding queried by `Device`. The GLFW/OpenGL integration
requests an sRGB attachment by default and validates what the window system
actually provided. A mismatch is reported before rendering rather than being
silently approximated.

An OpenGL `Frame` retains a lightweight device facade instead of a pointer to
the caller's `Device` object. A context admits one active frame scope. Moving a
frame transfers that scope; ending or destroying it revokes its command stream.
Frame and command validity use context-owned integer generations, so beginning
a frame or obtaining its command stream does not allocate a control block.
`end()` does not present or stall because immediate-mode OpenGL work has already
been issued. Presentation remains the explicit `window.present()` step.

## Frame commands

An OpenGL renderer borrows the frame's context and chooses state where needed:

```cpp
auto context = frame.render_context();
auto graphics = context.graphics_state();

// Check std::expected results in application code.
graphics.set(vng::render::DepthState{
    .test = true, .write = true,
    .compare = vng::render::DepthCompare::less,
});
graphics.set(vng::render::FrontFace::counter_clockwise);
graphics.set(vng::render::BlendMode::disabled);
graphics.set(vng::opengl::PolygonMode::fill);
context.run(program_);
context.view(view);

for (const CharacterDraw& draw : draws) {
    graphics.set(draw.selected
        ? vng::render::CullMode::none
        : vng::render::CullMode::back);
    context.draw(mesh_for(draw.character));
}
```

The available operations have separate responsibilities:

- `run(program)` selects shaders, avoiding redundant native selection;
- `bind(program)` explicitly rebinds shaders without changing graphics settings;
- `view(view)` uploads the camera snapshot requested by that program;
- `graphics.set(setting)` changes a supported setting for subsequent draws;
- `graphics.snapshot()` returns a value copy of managed backend settings;
- `draw(gpu_mesh, instance_count)` checks the semantic vertex contract,
  obtains/caches the matching VAO, and issues the draw.

The backend-neutral graphics-state facade statically forwards to backend
overloads. Portable renderers use portable setting types; specialized renderers
can additionally use backend extensions. A future backend with immutable
native pipelines can cache implementations at draw time without exposing that
mechanism as persistent user policy.

## Lifetime and composition

All command handles obtained from a frame share one live command context.
Requesting another handle, calling a child renderer, dropping a handle or moving
the frame does not revoke existing handles. Ending or destroying the frame owner
does. A draw requires a selected program and, when its shader reads the camera,
a matching `RenderView` uploaded for that selection.

Binding a temporary program is rejected at compile time. Moving or destroying
the selected program clears the selection safely; select a live program again
before drawing.

Parent and child renderers change the same program and graphics state; there is
no automatic restoration when a child returns. Every renderer establishes the
settings it needs. A parent reselects its own program and state before continuing,
optionally using an explicit graphics-state snapshot to restore those settings.

Expert calls can change native state behind the managed cache.
`context.restore()` reasserts desired graphics state, program and frame encoding;
it does not repair native framebuffer or viewport changes. Raw
`Program::bind()` clears managed selection, so explicitly select the program
again afterward.

An explicit `RenderStateScope` restores native GL state and invalidates managed
binding/view and graphics caches. It does not revoke command handles or silently
restore a separate managed-state snapshot: reselect the program and required
settings after the scope before drawing.

Output encoding belongs exclusively to the frame/target, not to compiled shaders.

## Shader and diagnostic runtime

`OpenGLProgramRuntime` is a resource owned by a concrete OpenGL renderer.
It does not interpret tickets or choose draw order. It owns the ordinary
compiled program plus lazily compiled enhanced variants from the same shader IR:

```cpp
auto runtime = vng::render::OpenGLProgramRuntime::create(
    device, std::move(shader_program));

// Ordinary rendering:
context.run(runtime->production());
context.view(view);
context.draw(gpu_mesh);

// Capture this invocation's current state:
auto evidence = runtime->capture(frame, cpu_mesh, gpu_mesh, view);
```

The frame overload snapshots live managed graphics settings and target encoding.
Device-only capture has no frame from which to obtain those values and therefore
requires explicit `opengl::CaptureState{graphics_snapshot, target_encoding}`.
Neither path inherits shader-creation-time state.

The runtime handles enhanced shader emission, private analysis targets,
readback, and evidence construction. The renderer remains responsible for
interpreting its tickets and establishing the same per-invocation state for
ordinary and diagnostic drawing. Unsupported diagnostic configurations return
errors rather than silently producing a different draw.

## Small concrete renderers

`make_simple_mesh_renderer(device, program, mesh)` creates a concrete
`opengl::SimpleMeshRenderer` for a program needing one owned mesh and program.
It is an explicit convenience, not behavior hidden inside `Renderer<Ticket, Backend>`.

The file-mesh demo is the primary composition example:
`FileMeshRenderer : opengl::Renderer<FileMeshDraw>` owns shader creation and resources,
and selects depth/culling from tickets. Its factory receives the device and CPU
mesh, not a precompiled shader or retained state preset.
`vng_file_mesh_direct_demo` is the side-by-side example where the application
deliberately owns the shaders and commands itself.

See [program compilation](graphics_program.md) and
[live graphics state](graphics_state.md) for their respective boundaries.
