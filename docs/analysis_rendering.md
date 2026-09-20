# Analysis rendering

The renderer/emission path described here feeds the backend-neutral inspection,
comparison, visualization, and export layer in
[`analysis_evidence.md`](analysis_evidence.md).

Analysis is emitted from the ordinary shader IR; it is not a second shader the
application must maintain. One linked `shader::GraphicsProgram` can produce:

```cpp
auto normal = vng::glsl::emit(program);
auto surfaces = vng::glsl::emit(program, vng::glsl::AnalysisEmission{});
auto normals = vng::glsl::emit(program, vng::glsl::observation(WorldNormal{}));
```

The normal product is unchanged. The surface product preserves the normal color
outputs and adds renderer-owned identity:

```glsl
// Generated in the vertex shader.
layout(location = C) uniform uint vng_analysis_first_item_id;
flat out uint vng_analysis_item_id;

vng_analysis_item_id =
    vng_analysis_first_item_id + uint(gl_InstanceID);

// Generated in the fragment shader.
layout(location = N) out uvec2 vng_analysis_surface_key;

vng_analysis_surface_key =
    uvec2(vng_analysis_item_id, uint(gl_PrimitiveID));
```

The emitter chooses collision-free locations and returns them as metadata. It
does not edit GLSL text, replay the shader lambda, or mutate the IR.
`OpenGLProgramRuntime` emits and compiles each diagnostic product lazily, then
caches it. It is a shader/diagnostic resource owned by a concrete OpenGL
renderer, not a renderer itself. Creating a renderer and using only `render`
adds no diagnostic shader work or per-fragment cost.

## Application-facing use

The normal frame remains the short, target-scoped path. The file-mesh example
defines `FileMeshRenderer : opengl::Renderer<FileMeshDraw>`. Its factory compiles an
internally defined shader pair into an `OpenGLProgramRuntime`, uploads the
mesh, and retains both CPU topology and GPU storage. Application code never
supplies a diagnostic or production shader to this renderer. Its ordinary
render method remains in full control of commands:

```cpp
auto commands = frame.commands();
commands.run(shader_runtime_.production());
commands.view(view);

for (const FileMeshDraw& draw : draws) {
    commands.depth({
        .test = draw.depth_test,
        .write = draw.depth_write,
        .compare = vng::render::DepthCompare::less,
    });
    commands.cull(draw.cull);
    commands.draw(gpu_mesh_, draw.instance_count);
}
```

The calls above are shortened to show the policy; the executable checks every
returned `std::expected`. A renderer may bind several programs and change state
between arbitrary ticket groups. Analysis does not impose a one-program render
architecture.

For inspection, the caller describes wanted evidence rather than framebuffer
attachments or shader variants:

```cpp
auto evidence = renderer->capture(
    *frame, *view, file_mesh_draws,
    vng::analysis::CaptureRequest::standard());

if (auto hit = evidence->capture().surface_at({x, y})) {
    std::cout << "source face " << hit->face << '\n';
}
```

`RenderView` is the common immutable camera snapshot and target extent. Using
the same view for normal and diagnostic rendering prevents accidental camera or
aspect-ratio differences.

The CPU mesh supplies source face-to-vertex topology. `GpuMesh` retains a
deterministic 128-bit fingerprint of the vertex count and ordered triangle
indices from upload. Capture checks that fingerprint before source lookup,
catching an accidentally paired or subsequently edited topology. It is a
non-cryptographic mismatch detector and deliberately excludes attributes,
metadata, and authored edges. A later `MeshAsset` can own both resources.

Optional provenance and a human-readable label attach to the render item:

```cpp
auto evidence = shader_runtime_.capture(
    frame, cpu_mesh, gpu_mesh, *view,
    vng::analysis::CaptureRequest::standard(), analysis_options);
```

The explicit `AnalysisOptions` overload belongs to the lower-level program
runtime boundary for tools that need to attach entity/material provenance.
That data normally comes from a graphical instance or asset database; it does
not belong in `.vmesh`. `AnalysisOptions::clear_depth` is normally empty. The
runtime chooses `1` for ordinary depth and `0` for reverse depth.

The runtime stores shader products, not a raster preset. Frame overloads take
a snapshot of the current `commands.graphics_state()` at each invocation, so
set the desired draw settings before capture just as before drawing. Culling,
winding and depth decisions in evidence and diagnostic variants come from
that snapshot. Each counterfactual changes only the setting under test.
Low-level device-only overloads require an explicit `opengl::CaptureState` containing
the raster snapshot and target encoding. Output encoding belongs to the target,
never to the program.

## Selective shader observations

A shader exposes a meaningful internal value once by attaching a semantic:

```cpp
struct WorldNormal : vng::gfx::Semantic<vng::Vec3> {};

auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
    [](auto& stage) {
        auto normal = compute_world_normal(stage);
        stage.observe(WorldNormal{}, normal);
        return stage.output(/* ordinary outputs */);
    });
```

`observe` records IR metadata only. Ordinary GLSL remains byte-for-byte the
same, and a diagnostic-only expression does not become live merely because it
was named. A tool opts in through its request:

```cpp
auto request = vng::analysis::CaptureRequest::diagnostic()
    .observe(WorldNormal{});
auto evidence = renderer->capture(
    *frame, *view, draws, request);

auto pixel = evidence->inspect({x, y});
if (const auto* normal = pixel->observation(WorldNormal{})) {
    use(*normal);
}
```

Each requested semantic is emitted into its own cached shader product and read
back into a typed floating, signed-integer, or unsigned-integer image. It
arrives as an `EvidenceChannel`; the application never selects an OpenGL image
format, attachment, or GLSL type. Unrequested observations have no runtime
cost. `FrameEvidence` refuses a result if a requested canonical channel is
missing or its C++ logical type differs from the semantic. This slice
materializes fragment-stage scalar and vector observations.

## Diagnostic sweep

`diagnose` captures production, then repeats the same tickets, camera, shader
IR, and extent while changing one renderer-owned raster state at a time:

```cpp
auto sweep = renderer->diagnose(
    *frame, *view, draws,
    vng::analysis::CaptureRequest::diagnostic()
        .observe(WorldNormal{}));

for (const auto& finding : sweep->findings()) {
    std::cout << finding.message << '\n';
}
```

The current counterfactuals disable culling and force an always-pass depth
test. If either reveals coverage absent from production, the sweep reports a
strong winding/culling or depth-state hint. It also identifies covered-but-dark
output, non-finite covered depth, and submitted items with no visible pixels.
Before drawing conclusions, the analysis core verifies a structured invocation
identity generated by the renderer: the workload, view, renderer definition,
and logical target fingerprints must match across all variants. Separate frame
IDs and the `raster.variant` metadata are intentionally allowed to differ.
This is bounded diagnosis, not proof of every possible render bug, and belongs
in editor, test, or agent tooling rather than the hot frame loop.

The file-backed example demonstrates the complete route. It names the fragment
color as `SurfaceColor`, requests it, prints the center value and coverage for
all three raster variants, then continues through the ordinary visible render:

```text
./build/vng_file_mesh_demo --analyze --once
```

## Captured data and evidence

The canonical physical target contains linear `RGBA8` color, `RG32UI` surface
keys, and authoritative `DEPTH_COMPONENT32F` depth. `SurfaceKey` is a dense
frame-local item ID plus draw-local primitive ID. Persistent entity, asset,
revision, material, source-face, and source-vertex information stays in the
`AnalysisManifest`, not in every pixel.

OpenGL readback has bottom-left row order. The renderer flips every channel
into top-left order, so `inspect({x, y})` accepts ordinary UI coordinates.
`FrameEvidence` adds automatic summaries, optional semantic images, neutral
camera and shader-IR metadata, and backend artifacts such as generated GLSL,
source maps, pipeline state, and debug messages. It can be compared or exported
without an OpenGL context; see [`analysis_evidence.md`](analysis_evidence.md).

Diagnostic rendering snapshots the OpenGL state it changes and restores it on
success or failure. Its private framebuffer, viewport, program/VAO,
depth/cull/scissor/rasterizer state, framebuffer-sRGB state, and affected color
masks and blend state do not leak into the following normal frame. Augmented
programs retain the full emitted fragment interface, so this includes every
ordinary sparse MRT output as well as the injected diagnostic attachment;
unrelated draw-buffer slots are never normalized as collateral state.

## Current boundary

This slice is synchronous, single-sample, triangle-list, and opaque. One call
captures one mesh as one frame item. The canonical capture is one enhanced draw;
each requested observation and each counterfactual is another draw followed by
blocking readback. This explicit expense is isolated from normal rendering.

Canonical color is linear `RGBA8`, so it is stable for inspection but cannot
preserve HDR and need not equal an sRGB swapchain byte-for-byte. Blended
transparency and integer MSAA resolve do not have one honest surface owner and
are not claimed yet. Asynchronous readback, multi-item scene captures, and
derived world position or motion can be added without changing shader authoring.
Capture explicitly rejects blending, non-filled polygon modes, and depth tests
without depth writes rather than silently claiming evidence for a different
draw state.
