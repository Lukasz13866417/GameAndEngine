# Meshes and `.vmesh`

This slice deliberately separates three representations:

```text
human-readable .vmesh text
        |
        v
vmesh::Document             stable field names and logical values
        | vmesh::Schema
        v
gfx::Mesh<Record...>        typed, explicitly encoded runtime storage
        |
        v
backend upload              OpenGL now; other backends can consume the same Mesh
```

The parser does not know that `"position"` means a C++ `Position` type. The
runtime mesh does not carry strings through the render loop. A small schema at
the asset boundary connects the two once.

## Runtime mesh

Semantics and records are the same types used by the vertex-input system:

```cpp
struct Position : vng::gfx::Semantic<vng::Vec3> {};
struct Normal   : vng::gfx::Semantic<vng::Vec3> {};
struct Color    : vng::gfx::Semantic<vng::Vec4> {};

using Vertex = vng::gfx::Record<
    Position,
    Normal,
    vng::gfx::as<Color, vng::gfx::unorm8x4>>;

vng::gfx::Mesh<Vertex> mesh(3);
mesh.info().name = "colored triangle";

mesh.vertices()[0].set(Position{}, {-0.5F, -0.5F, 0.0F});
mesh.vertices()[0].set(Normal{},   { 0.0F,  0.0F, 1.0F});
mesh.vertices()[0].set(Color{},    { 1.0F,  0.0F, 0.0F, 1.0F});

// Fill vertices 1 and 2, then add zero-based triangle indices.
mesh.add_face(0, 1, 2);

// Optional authored edge topology. Calling this with no arguments means
// "the edge section is present and explicitly empty".
mesh.set_explicit_edges();
```

`Mesh<Record...>` may contain several synchronized per-vertex streams. This is
useful when position data and surface data need different update/storage
policies:

```cpp
using Geometry = vng::gfx::Record<Position, Normal>;
using Surface  = vng::gfx::Record<vng::gfx::as<Color, vng::gfx::unorm8x4>>;

vng::gfx::Mesh<Geometry, Surface> mesh(3);
auto& geometry = mesh.vertices<Geometry>();
auto& surface = mesh.vertices<Surface>();
```

All streams use one vertex-index domain and must have the same record count.
`mesh.validate()` checks that invariant and checks every face and edge index.
Per-instance data is intentionally not part of a mesh: it describes draw
instances, not mesh topology.

Faces are triangles in version 1. This makes the runtime object directly
renderable and keeps triangulation in an importer or authoring tool, where the
original polygon context still exists. Explicit edges are retained for tools or
gameplay; they are not inferred or uploaded by this slice.

## Text format

A complete file has a version header, required `info`, `vertices`, and `faces`
sections in that order, followed by an optional `edges` section:

```text
vmesh 1.0
info {
    name = "colored quad";
    source/tool = "hand-authored";
}
vertices 4 {
    fields {
        position : f32x3;
        normal : f32x3;
        color/0 : f32x4;
    }
    data {
        [-1, -1, 0] [0, 0, 1] [1, 0, 0, 1];
        [ 1, -1, 0] [0, 0, 1] [0, 1, 0, 1];
        [ 1,  1, 0] [0, 0, 1] [0, 0, 1, 1];
        [-1,  1, 0] [0, 0, 1] [1, 1, 1, 1];
    }
}
faces 2 {
    [0, 1, 2];
    [0, 2, 3];
}
edges 4 {
    [0, 1];
    [1, 2];
    [2, 3];
    [3, 0];
}
```

The rules for version 1 are intentionally small:

- Counts are explicit and must exactly match their sections.
- Vertex rows follow the field declaration order.
- Field types are `f32`, `i32`, or `u32`, optionally followed by `x2`, `x3`,
  or `x4`.
- Every scalar or vector value uses brackets. Commas and terminating semicolons
  are required.
- Face and edge indices are zero-based `u32` values and must be in range.
- Repeated face corners, self-edges, and duplicate faces or edges remain
  representable. They can be useful in generated data, and deciding whether
  they are mistakes belongs to an optional topology-quality validator rather
  than the serialization layer.
- Face index order is preserved, but version 1 does not declare one winding to
  be front-facing; that is renderer raster state. Edge endpoint order is also
  preserved without assigning directed-edge meaning. Coordinate values are
  semantic data, so handedness and up-axis conversion belongs at the importer
  boundary until the engine chooses a world-space convention.
- The edge section is optional. Omitted edges and `edges 0 {}` remain distinct
  in both `Document` and `Mesh`.
- `#` starts a line comment. CRLF and LF line endings are accepted.
- Metadata values are quoted UTF-8 strings with JSON-style escapes. Metadata
  and field names are stable slash-separated identifiers such as `color/0`;
  C++ type spellings never become file identifiers.
- Floating-point input must be finite. The writer emits enough digits to
  preserve every accepted `f32` value exactly.
- Parsing is bounded by configurable source, decoded-byte, token, entry, and
  primitive-count limits before count-driven allocations are attempted.

The format stores logical values, not target storage encodings. For example,
`color/0 : f32x4` can decode into either an ordinary `Color` field or a
`gfx::as<Color, gfx::unorm8x4>` field. `f16` and normalized integer encodings
belong to the runtime `Record`, not this source format.

## Typed decoding and encoding

Define the stable-name mapping once per record shape. The mutable form reads as
a direct mapping from file names to typed semantics:

```cpp
namespace vmesh = vng::content::vmesh;

auto vertex_schema = vmesh::schema<Vertex>();
vertex_schema.map("position", Position{});
vertex_schema.map("normal", Normal{});
vertex_schema.map("color/0", Color{});

auto mesh = vmesh::load("quad.vmesh", vertex_schema);
if (!mesh) {
    // mesh.error() includes an error code, message, optional source location,
    // path, and notes.
}
```

For an inspection tool, keep the explicit intermediate representation:

```cpp
auto document = vmesh::read_vmesh("quad.vmesh");
if (document) {
    auto mesh = vmesh::decode(*document, vertex_schema);
}
```

Each `map` call is compile-time checked to ensure that the semantic belongs to
`Vertex` and that its logical value type is supported by `.vmesh`. A mutable
object cannot encode its number of completed calls in its C++ type, so missing
semantics and repeated calls for the same semantic are diagnosed when
`decode`, `encode`, `load`, or `save` uses the schema. File names are checked at
the same boundary because they are owned strings. Mapping call order determines
field order when encoding. Extra fields in a document are legal and ignored by
a schema, which lets a consumer select the data it needs.

The original compact form remains available. Because all mappings are part of
the construction expression, it checks completeness and unique semantic
coverage at compile time:

```cpp
const auto vertex_schema = vmesh::schema<Vertex>(
    vmesh::map("position", Position{}),
    vmesh::map("normal", Normal{}),
    vmesh::map("color/0", Color{}));
```

Encoding follows the reverse path. `save` is the compact form:

```cpp
auto saved = vmesh::save("copy.vmesh", *mesh, vertex_schema);

// Or inspect/change the intermediate representation:
auto document = vmesh::encode(*mesh, vertex_schema);
if (document) {
    auto text = vmesh::write_vmesh(*document);
}
```

`info.name` is the one reserved metadata mapping in version 1 and maps to
`mesh.info().name`. Other metadata remains available on `vmesh::Document`; it
is not silently carried into the render-time mesh. Consequently, typed
`load` followed by `save` is a projection rather than a lossless document edit:
unselected fields and metadata other than `name` are dropped, and packed
runtime codecs can quantize values. Use `Document` when a tool must preserve
all represented fields and metadata. This is not a byte-for-byte text roundtrip:
the writer canonicalizes ordering and formatting, and comments are not part of
`Document`.

For multiple streams, pass one schema per stream in the same order as the
`Mesh` record parameters:

```cpp
auto geometry_schema = vmesh::schema<Geometry>();
geometry_schema.map("position", Position{});
geometry_schema.map("normal", Normal{});

auto surface_schema = vmesh::schema<Surface>();
surface_schema.map("color/0", Color{});

auto mesh = vmesh::decode(*document, geometry_schema, surface_schema);
```

The parser first builds an owned, columnar `Document`. Schema decoding indexes
field names once, then writes through compile-time `Record::set` operations.
There is no virtual accessor, runtime semantic lookup, or file-format state in
the rendering hot path.

## Application rendering

The ordinary application path gives the typed mesh to a concrete renderer that
owns its persistent resources and policy. For example, the file-mesh demo's
`FileMeshRenderer : opengl::Renderer<FileMeshDraw>` compiles its program runtime and
uploads the mesh in its fallible factory:

```cpp
auto cpu_mesh = vmesh::load("quad.vmesh", vertex_schema);
if (!cpu_mesh) {
    // Report cpu_mesh.error().
}

auto renderer = FileMeshRenderer::create(
    device, std::move(*cpu_mesh), baseline_state);

const std::array draws{FileMeshDraw{}};
auto drawn = renderer->render(frame, view, draws);
```

Inside that renderer, `opengl::upload_mesh` creates its `GpuMesh`, and
its private shader factory creates and links the program consumed by
`OpenGLProgramRuntime`. Application code supplies neither stage. The
`frame.commands().draw(gpu_mesh)` uses the currently bound program's typed
semantic/location metadata to derive the necessary vertex bindings. The
factory prewarms the matching VAO, so an incompatible mesh/program pair fails
during construction rather than on its first draw. Direct `GpuMesh` use can
still resolve layouts lazily, and a different compatible program layout gets a
separate cached VAO. Split or reordered CPU
streams and packed record codecs therefore remain invisible to ticket
submission. A mesh missing one of the program's vertex semantics is rejected
instead of being partially bound.

`opengl::upload_mesh` and `GpuMesh` are backend resources intended to live
inside an OpenGL renderer. Direct `GpuMesh::draw` remains a lower-level escape
hatch; the usual custom-renderer path draws through `Frame::commands`, which
also tracks the bound pipeline, view, and dynamic state. Neither route exposes
raw buffers or VAO construction.

For tiny programs that genuinely want exactly one owned mesh and one pipeline,
`make_simple_mesh_renderer` constructs an optional concrete convenience. The
triangle demo uses it; the file-mesh demo is the reference for defining a
renderer with its own ticket and on-the-fly state policy.

An OpenGL upload is an immutable GPU snapshot. Changing the CPU mesh does not mutate
it; upload again to produce a new snapshot. `MeshInfo` and explicit edges stay
CPU-side, while the synchronized per-vertex streams and triangle faces are
uploaded. Both `Mesh` and its `GpuMesh` snapshot expose a matching
`topology_fingerprint()`: a deterministic 128-bit fingerprint over vertex count
and the ordered triangle indices. This lets tooling and analysis paths reject a
CPU mesh that is not the source topology of a GPU snapshot, even when both have
the same number of faces. It intentionally ignores vertex values, metadata, and
explicit edges. The fingerprint guards against accidental mismatches; because
it is non-cryptographic, it is not proof against deliberately constructed
collisions.

Faces are uploaded as a `u32` triangle index buffer. Explicit edges remain on
the CPU because they are authored/tooling topology, not automatically a second
rendering primitive. The optional third argument to `draw` repeats the mesh for
instancing; typed per-instance records are separate draw data and remain a
later composition layer rather than becoming part of `Mesh`.

Handwritten GLSL is still supported by the low-level OpenGL API, but automatic
semantic binding requires an unmodified GLSL artifact emitted by the typed
pipeline. This keeps the convenient path tied to a trustworthy input contract.

The complete file-backed path is executable in
[`examples/file_mesh.cpp`](../examples/file_mesh.cpp), with its custom renderer
kept separately in
[`examples/file_mesh_renderer.hpp`](../examples/file_mesh_renderer.hpp) and
[`examples/file_mesh_renderer.cpp`](../examples/file_mesh_renderer.cpp). It uses
the editable
[`examples/assets/colored_cube.vmesh`](../examples/assets/colored_cube.vmesh)
asset. Its optional first command-line argument selects another compatible
`.vmesh` file. The adjacent
[`examples/file_mesh_direct.cpp`](../examples/file_mesh_direct.cpp) renders the
same asset without any renderer object and exposes shader creation, pipeline
compilation, upload, binding, and drawing at the call site.

## Deliberately deferred

Version 1 does not attempt to be a DCC interchange format. Polygon faces,
per-corner attributes, skinning conventions, per-face/per-edge fields, mesh
subsets, materials, compression, and a cooked binary format need explicit
design rather than accidental extensions to this grammar. A future importer can
convert DCC data into this render-ready representation, duplicating vertices at
UV, normal, or material seams before writing triangle faces.
