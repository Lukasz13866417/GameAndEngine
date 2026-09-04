# Renderers, frames, and backend commands

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
    : public vng::render::Renderer<CharacterDraw> {
public:
    std::expected<void, vng::opengl::Diagnostic> render(
        vng::opengl::Frame& frame,
        const vng::render::RenderView& view,
        std::span<const CharacterDraw> draws);

private:
    // Programs, pipelines, GPU meshes, staging storage, caches, ...
};
```

`Renderer<Ticket>` is only an empty, zero-state, non-virtual user base. It supplies
`ticket_type` and gives generic orchestration an unambiguous renderer identity.
It does not own an implementation, forward calls, select a backend, or impose
one shader/pipeline on the derived class. Its protected construction and
destruction also prevent accidentally instantiating a meaningless plain
renderer.

`RendererType<T>` recognizes a concrete public derivation, while
`RendererFor<T, Frame>` checks that it provides the canonical batch call for a
particular frame type. Both checks are compile-time only; there is no virtual
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

## Ownership and construction

The concrete renderer chooses its constructor or fallible factory. Backend
selection is therefore explicit where persistent backend resources are made:

```cpp
auto renderer = CharacterRenderer::create(
    device, shader_program, character_assets);
```

For an OpenGL renderer, creation can compile programs/pipelines and upload
meshes. A renderer for another backend owns that backend's corresponding
objects. Nothing requires every renderer to use the same factory shape.

This also permits different levels of specialization:

- a conventional renderer can own neutral descriptions and realize them for
  its selected backend during `create`;
- an OpenGL-specific renderer can use OpenGL frame commands and backend
  resources directly;
- a renderer can support more than one frame type with constrained overloads
  if sharing its policy is actually useful.

Tickets should stay cheap and describe changing submission data. Shader
programs, GPU buffers, pipelines, caches, and other persistent objects belong
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

## Renderer-controlled OpenGL commands

An OpenGL renderer obtains a frame-scoped command stream and makes state
decisions at the point where they matter:

```cpp
auto commands = frame.commands();

if (auto result = commands.bind(opaque_pipeline_); !result) {
    return result;
}
if (auto result = commands.view(view); !result) {
    return result;
}

for (const CharacterDraw& draw : draws) {
    if (draw.selected) {
        if (auto result = commands.cull(vng::render::CullMode::none);
            !result) {
            return result;
        }
    } else {
        if (auto result = commands.cull(
                vng::render::CullMode::back,
                vng::render::FrontFace::counter_clockwise);
            !result) {
            return result;
        }
    }

    if (auto result = commands.depth({
            .test = true,
            .write = true,
            .compare = vng::render::DepthCompare::less,
        }); !result) {
        return result;
    }

    if (auto result = commands.draw(mesh_for(draw.character)); !result) {
        return result;
    }
}
```

The available operations are deliberately small:

- `bind(pipeline)` selects a compiled shader and establishes its complete
  baseline state;
- `view(view)` uploads parameters requested by that DSL program;
- `state(description)` replaces all portable fixed-function choices;
- `depth(...)` and `cull(...)` change individual choices on the fly;
- `draw(gpu_mesh, instance_count)` validates the active program's semantic
  vertex contract, obtains/caches the matching VAO, and issues the draw.

This API maps naturally to mutable OpenGL state. A future backend with mostly
immutable native pipelines can implement its command layer by treating the
current portable state as a pipeline-cache key at `draw`; renderer policy does
not need to pretend that every draw has one permanent state object.

Only the newest command stream obtained from a frame remains valid. Requesting
another stream, moving the frame, ending it, or destroying it revokes the old
stream. A draw also requires a bound pipeline and, when its shader reads the
camera, a matching `RenderView` uploaded since that bind.

The command stream keeps a non-owning reference to its bound pipeline. Binding
a temporary is rejected at compile time; the renderer-owned pipeline must stay
alive and unmoved while that command stream is used.

Within a command stream, its program and fixed-function state are an exclusive
scope. Expert calls such as direct `GraphicsPipeline::bind`, `GpuMesh::draw`,
device state setters, or native OpenGL can change ambient state behind its
back. After such an escape hatch, call `commands.bind(...)` and establish the
desired state again before continuing through the command stream.

`GraphicsPipelineDesc::output_encoding` is the compiled pipeline's target
expectation. It cannot be changed to a different encoding through
`Commands::state` because the frame's physical target has already been chosen.

## Shader and diagnostic runtime

`OpenGLProgramRuntime` is a resource used by a concrete OpenGL renderer. It is
not itself a renderer and does not interpret tickets or choose draw order. It
owns the ordinary compiled pipeline plus lazily compiled enhanced variants
emitted from the same shader IR:

```cpp
auto runtime = vng::render::OpenGLProgramRuntime::create(
    device, std::move(shader_program), baseline_state);

// In the renderer's ordinary render method:
commands.bind(runtime->normal_pipeline());
```

A renderer that offers diagnostic capture can retain the CPU source mesh, its
matching `GpuMesh`, and this runtime, then forward its own validated ticket to
`runtime.capture(...)` or `runtime.diagnose(...)`. The runtime handles enhanced
shader emission, private analysis targets, readback, and evidence construction;
the concrete renderer remains responsible for deciding what its ticket means.

## Optional simple renderer

`make_simple_mesh_renderer` is an explicit convenience for a tiny program that
really does want one owned mesh, one program, and one baseline pipeline:

```cpp
auto renderer = vng::render::make_simple_mesh_renderer(
    device,
    std::move(program),
    std::move(mesh),
    pipeline_description);

const vng::render::MeshDraw draw{};
vng::render::render_one(*renderer, frame, view, draw);
```

This creates the concrete `opengl::SimpleMeshRenderer`; it is not implicit and
does not constrain custom renderers. The triangle demo uses it to stay small.
The file-mesh demo is the primary architecture example: its
`FileMeshRenderer : Renderer<FileMeshDraw>` owns resources and changes depth
and culling from ticket data through `Frame::commands()`. Its factory also owns
shader-stage creation; the application passes a mesh and baseline policy, not
a shader program. `vng_file_mesh_direct_demo` is the side-by-side lower-level
example where the caller intentionally owns those shaders and commands.

## Pipeline realization

`GraphicsPipelineDesc` remains useful as portable baseline state, independent
of renderer structure:

```cpp
auto pipeline = vng::render::compile_pipeline(
    device, shader_program, pipeline_description);
```

For an OpenGL device this returns an `opengl::GraphicsPipeline`; another
backend supplies its own realization. A renderer may own several such
pipelines, bind different ones in one call, and apply temporary state changes
between draws. See [`graphics_pipeline.md`](graphics_pipeline.md) for the
detailed compilation boundary.
