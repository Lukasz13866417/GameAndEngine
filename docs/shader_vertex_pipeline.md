# Shader and vertex pipeline

## Boundaries

There are three deliberately separate contexts:

1. A shader stage's `FunctionBuilder` records backend-neutral operations.
2. The GLSL emitter lowers a completed, linked `GraphicsProgram` without a live
   graphics context.
3. `opengl::GpuMesh` combines a typed mesh with the linked program's input
   contract and asks `opengl::Device` to create the required buffers and VAO
   while a context supplied by a window backend is current.

`Expr<T>` is only a typed `{FunctionBuilder*, ValueId}` handle. Its copy
operations are inert; nodes and their operand IDs live in the module arena.
`ValueId` carries an opaque, process-unique builder generation in addition to
its table index, so an escaped value cannot become valid again if a later
builder reuses the same address. Expressions are nevertheless callback-scoped
handles and should not be retained after their builder has gone away. The IR
stores regions and effect classifications now, even though dynamic control
flow and shader-visible writes are postponed.

## Semantic vertex records

```cpp
struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};
struct InstanceOffset : vng::gfx::Semantic<vng::Vec2> {};

using Vertex = vng::gfx::Record<
    Position,
    vng::gfx::as<Color, vng::gfx::unorm8x4>>;

using Instance = vng::gfx::Record<InstanceOffset>;

using Layout = vng::gfx::VertexLayout<
    vng::gfx::Stream<Vertex, vng::gfx::PerVertex>,
    vng::gfx::Stream<Instance, vng::gfx::PerInstance<1>>>;
```

The semantic type defines identity and logical type. A field occurrence chooses
physical encoding. `Record` calculates offsets and stride from codec metadata
and owns only encoded bytes:

```cpp
vng::gfx::VertexStream<Vertex> vertices(3);
vertices[0].set(Position{}, {-0.25F, -0.5F});
vertices[0].set(Color{}, {1.0F, 0.0F, 0.0F, 1.0F});

auto decoded = vertices[0].get(Color{}); // Vec4
auto upload = vertices.bytes();
```

Internally, `resolve_vertex_input<ShaderInputs, Layout>()` walks shader semantics
in shader-location order and finds their stream, byte offset, stride, rate, and
codec. Its result contains no OpenGL enum. The OpenGL bridge subsequently
selects floating, normalized, or integer attribute delivery. Application code
does not call either operation while submitting tickets: a concrete OpenGL
renderer owns the `GpuMesh`, binds a pipeline through `Frame::commands()`, and
lets `commands.draw(...)` perform/cache setup from the actual linked program.
`GpuMesh::draw(device, pipeline)` remains a backend-level escape hatch;
explicit resolution remains the advanced path for standalone or per-instance
streams.

## Shader construction

```cpp
using VertexIn = vng::shader::VertexInputs<Position, Color, InstanceOffset>;
using VertexOut = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<Color>>;
using FragmentIn = vng::shader::FragmentInputs<vng::shader::smooth<Color>>;
using FragmentOut = vng::shader::FragmentOutputs<vng::shader::Color<0>>;

using vng::dsl::field;
using vng::dsl::vec4;
using vng::shader::ClipPosition;

auto vertex = vng::shader::vertex<VertexIn, VertexOut>([](auto& stage) {
    auto position = stage.input(Position{});
    auto offset = stage.input(InstanceOffset{});
    auto color = stage.input(Color{});
    auto clip = vec4(position + offset, 0.0F, 1.0F);

    return stage.output(
        field<ClipPosition>(clip),
        field<Color>(color));
});

auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>([](auto& stage) {
    return stage.output(
        field<vng::shader::Color<0>>(
            stage.input(Color{})));
});
```

`input(Tag{})` is the concise form of `inputs().get(Tag{})`. A zero-argument
`input<Tag>()` overload is also available. Inside a generic stage lambda that
spelling needs C++'s `stage.template input<Tag>()` disambiguator, which is why
the examples prefer the tag-taking form. `output(...)` constructs the stage's
already-declared output record, so its type does not need to be repeated. The
named `field<Tag>(value)` arguments still prevent two same-typed outputs from
being accidentally exchanged. These are only inline front-end conveniences:
the resulting `Expr<Outputs>` and IR are identical to calling
`dsl::make<Outputs>` directly.

The stage lambda executes immediately. Input reads, constants, constructors,
arithmetic, record construction, and field extraction append typed operations.
The output record is projected into stage-output operations, validated, and
dead pure operations are removed. `shader::link` then matches varying semantics
and interpolation and assigns deterministic locations across both stages.

A 3D vertex stage can request the renderer's camera without declaring a vertex
attribute or writing a backend uniform:

```cpp
field<ClipPosition>(stage.camera().project(stage.input(Position{})))
```

This records a typed read-only `camera_view_projection` parameter in the IR.
Program linking assigns its location, GLSL emission describes that location in
`ProgramSource::parameters`, and an OpenGL renderer uploads the current camera
snapshot with `commands.view(view)`. Camera-free shaders produce the same IR
and GLSL as before.

`dump_ir`, `dump_interface`, `gfx::dump_vertex_layout`, and the GLSL emitter's
returned source expose every transition for inspection. Each emitted stage also
carries erased interface metadata (semantic and GLSL type, location,
interpolation, builtin, and builtin index), so a backend need not retain or
reconstruct the original C++ schema to inspect its contract.

## One shader through the explicit stream path

The explicitly low-level `vng_advanced_instanced_streams_demo` uses the shader
above. `vng_triangle_demo` instead uses the deliberately narrow
`make_simple_mesh_renderer` convenience, with upload, pipeline compilation, and
inferred vertex input owned by its concrete OpenGL renderer. The primary
custom-renderer example is `vng_file_mesh_demo`, where
`FileMeshRenderer : Renderer<FileMeshDraw>` (in the adjacent focused renderer
files) owns its shader definitions and controls frame commands itself. The
same explicit shader ownership is shown outside a renderer by
`vng_file_mesh_direct_demo`. In every case the vertex lambda is invoked once,
synchronously, by
`shader::vertex`; its C++ temporaries are only `Expr<T>` handles. For example,
`position + offset` records one `add` operation and copying the returned handle
records nothing.

After stage construction and linking, the relevant vertex IR is:

```text
%0 : Vec2 = input [Position]
%1 : Vec2 = input [InstanceOffset]
%2 : Vec4 = input [Color]
%3 : Vec2 = add %0 %1
%4 : f32 = constant 0
%5 : f32 = constant 1
%6 : Vec4 = construct %3 %4 %5
%7 : VertexOut = construct %6 %2
%8 : Vec4 = extract_field %7 [ClipPosition]
stage_output %8 [ClipPosition]
%9 : Vec4 = extract_field %7 [Color]
stage_output %9 [Color]
return
```

IDs in the real `dump_ir()` are numeric and semantic/type names are fully
qualified. The symbolic labels above make the relationship easier to scan;
the shown order is the demo's GCC build. Independent nested C++ arguments may
be evaluated in a different order by another conforming compiler, but a built
IR is emitted repeatably and operand order is preserved. Linking assigns
attribute locations 0, 1, and 2 in `VertexIn` declaration order and varying
location 0 to `Color`.

The corresponding resolved CPU-side layout is:

```text
vertex input: 3 attributes, 2 streams
  location 0: Position,       binding 0, offset 0, stride 12, divisor 0, f32x2 (float)
  location 1: Color,          binding 0, offset 8, stride 12, divisor 0, u8x4 (normalized)
  location 2: InstanceOffset, binding 1, offset 0, stride  8, divisor 1, f32x2 (float)
```

GLSL lowering is independent of that physical layout. In particular, the
shader still sees `Color` as `vec4`:

```glsl
#version 460 core

layout(location = 0) in vec2 vng_in_0;
layout(location = 1) in vec4 vng_in_1;
layout(location = 2) in vec2 vng_in_2;
layout(location = 0) smooth out vec4 vng_out_1;

void main()
{
    vec2 v0 = vng_in_0;
    vec2 v1 = vng_in_2;
    vec4 v2 = vng_in_1;
    vec2 v3 = (v0 + v1);
    float v4 = 0.0;
    float v5 = 1.0;
    vec4 v6 = vec4(v3, v4, v5);
    // Record construction/extraction temporaries omitted here.
    gl_Position = v6;
    vng_out_1 = v2;
    return;
}
```

Finally, `opengl::configure_vertex_input` translates the neutral descriptors
`instance_buffer`, and `vao` below stand for their native handles:

```text
glVertexArrayVertexBuffer(vao, 0, vertex_buffer,   0, 12)
glVertexArrayBindingDivisor(vao, 0, 0)
glVertexArrayVertexBuffer(vao, 1, instance_buffer, 0,  8)
glVertexArrayBindingDivisor(vao, 1, 1)

glVertexArrayAttribFormat(vao, 0, 2, GL_FLOAT,         false, 0)
glVertexArrayAttribBinding(vao, 0, 0)
glEnableVertexArrayAttrib(vao, 0)

glVertexArrayAttribFormat(vao, 1, 4, GL_UNSIGNED_BYTE, true,  8)
glVertexArrayAttribBinding(vao, 1, 0)
glEnableVertexArrayAttrib(vao, 1)

glVertexArrayAttribFormat(vao, 2, 2, GL_FLOAT,         false, 0)
glVertexArrayAttribBinding(vao, 2, 1)
glEnableVertexArrayAttrib(vao, 2)
```

The split/reordered test changes the CPU record encoding, uploaded bytes,
stream topology, resolved layout, and consequently this binding state. It uses
the same linked IR and byte-for-byte identical generated shaders.

## OpenGL ownership

The GLFW/OpenGL integration creates and owns the paired window and context. It
exports a small current-context token consisting of a procedure resolver, a
current-context check, thread identity, and shared lifetime marker. The OpenGL
backend itself knows nothing about GLFW:

```cpp
auto window = vng::glfw_opengl::create_window(window_desc, context_desc);
auto current = window->make_current();
auto device = vng::opengl::Device::create(*current);
```

OpenGL objects are move-only and retain the context state. Public factories and
operations return `std::expected`; shader failures retain the complete generated
source and driver log. Resource factories validate API limits and turn errors
from allocation or attachment calls into typed diagnostics. Multiple `Device`
facades created from the same context token share one context state, debug sink,
and callback. Native destruction requires the owning context to remain current,
and a violated lifetime is retained as a device lifecycle diagnostic instead of
invoking GL through a dead context.

## Mesh and content consumer

That next layer is now implemented:

```text
.vmesh text -> vmesh::Document -> vmesh::Schema<Record...>
            -> gfx::Mesh<Record...> -> opengl::GpuMesh<Record...>
            + bound opengl::GraphicsPipeline -> cached VAO + indexed draw
```

The source file only knows stable field names and logical values. The schema
performs the one-time name-to-semantic mapping, the runtime records choose
physical codecs, and the linked program supplies input locations. None of the
file parsing machinery or names remain in the draw-time semantic lookup path.
See [mesh_and_vmesh.md](mesh_and_vmesh.md) for the grammar and complete API.

The same canonical shader IR can also be emitted as an inspection variant by
an `OpenGLProgramRuntime` owned by the concrete renderer, without authoring
another shader. See
[analysis_rendering.md](analysis_rendering.md) for the enhanced emission,
surface-key target, and source-face lookup path.
