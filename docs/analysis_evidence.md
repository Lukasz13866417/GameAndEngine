# Portable frame evidence

The analysis core turns renderer readback into deterministic, inspectable frame
evidence. It depends on engine core/gfx data, but not OpenGL, GLSL, a window,
or a renderer implementation. A backend is responsible only for producing the
canonical images and manifest; all summarizing, inspection, comparison,
visualization, and export below is CPU-only.

## Requests

Callers state intent with a small preset rather than configuring framebuffer
attachments:

```cpp
using vng::analysis::CaptureRequest;

auto ordinary = CaptureRequest::standard();
auto debug = CaptureRequest::diagnostic()
    .observe(WorldNormal{})
    .observe(Roughness{});
auto one_pixel = CaptureRequest::probe({mouse_x, mouse_y})
    .observe(WorldNormal{});
```

`standard` asks for canonical color, device depth, and surface identity.
`diagnostic` asks for the same authoritative data plus coverage, item, and
primitive visualizations. `observe(Semantic{})` selects named values explicitly
exposed by shader observation metadata. `probe` can select the same rich values
but marks one pixel as the requested scope. The request never exposes arbitrary
shader temporaries: shader authors choose the meaningful semantics that may be
observed, and callers choose which of those they want. Repeating a semantic is
idempotent and first-request order is retained. `FrameEvidence::create`
validates the probe against the result extent. It also treats every requested
observation as a contract: exactly one channel must exist at the semantic's
canonical `shader/...` name and its image must have exactly the semantic's C++
logical type. An absent `Vec3` observation, or one delivered as `Vec4`, fails
construction instead of producing incomplete evidence.

These are logical requirements. They do not prescribe MRTs, image formats,
passes, synchronization, or a shader language. A backend can satisfy them from
one enhanced draw, several draws, cached CPU data, or a future ray query.

## Canonical capture and summary

The existing `AnalysisCapture::create` API remains the boundary for the three
canonical top-left-origin images:

```cpp
auto capture = AnalysisCapture::create(
    std::move(color),
    std::move(device_depth),
    std::move(surface_keys),
    std::move(manifest));
```

Construction validates every surface key and automatically produces a
`CaptureSummary`. No second image traversal is needed at the call site:

```cpp
const auto& summary = capture->summary();

std::cout << summary.covered_pixel_count << '\n';
std::cout << summary.visible_item_count << '\n';
std::cout << summary.visible_primitive_count << '\n';
```

The summary records total/covered/background pixels, visible item and primitive
counts, inclusive covered bounds, finite covered-depth range and locations,
non-finite depth counts, and the corresponding values per manifest item.
Invisible manifest items remain in `summary.items` with zero coverage, which is
useful when diagnosing culling or occlusion.

`surface_at(pixel)` remains available and keeps its original optional behavior.
For tools, `inspect(pixel)` is more explicit:

```cpp
auto inspected = capture->inspect({x, y});
if (!inspected) {
    // The coordinate was out of bounds.
} else if (inspected->background()) {
    // Color, clear depth, and the canonical background key remain available.
} else {
    std::cout << inspected->surface->face << '\n';
}
```

Unlike `surface_at`, inspection distinguishes an invalid coordinate from a
valid background pixel and always returns the raw color/depth/key values.

## Frame metadata and additional channels

`FrameEvidence` combines a capture, its request, portable metadata, optional
typed diagnostic channels, and optional backend text artifacts:

```cpp
Image<Vec3> world_normals(extent);

auto evidence = FrameEvidence::create(
    std::move(*capture),
    CaptureRequest::diagnostic().observe(WorldNormal{}),
    FrameEvidenceMetadata{
        .label = "editor viewport",
        .renderer = "opaque geometry",
        .invocation = RenderInvocationIdentity{
            .workload_fingerprint = workload_fingerprint,
            .view_fingerprint = view_fingerprint,
            .renderer_fingerprint = renderer_fingerprint,
            .target_fingerprint = target_fingerprint,
        },
        .camera = camera.snapshot(extent).value(),
        .shader = ShaderEvidence{
            .program_name = "lit mesh",
            .vertex_ir = program.vertex().dump_ir(),
            .fragment_ir = program.fragment().dump_ir(),
            .interface_description = program.dump_interface(),
        },
        .properties = {{"scene/revision", "81"}},
    },
    {EvidenceChannel{
        observation_channel_name(WorldNormal{}),
        std::move(world_normals)}},
    {
        BackendArtifact{
            .name = "shader/vertex.glsl",
            .media_type = "text/x-glsl",
            .text = generated.vertex.source,
        },
        BackendArtifact{
            .name = "pipeline.txt",
            .media_type = "text/plain",
            .text = pipeline_description,
        },
    });
```

Additional channels support signed/unsigned/floating scalar and 2–4 component
vector images plus RGBA8. This is the extension point for later enhanced shader
emission: world position, world normal, UV, material classification, motion,
or game-specific semantic values do not require another capture class.
Channel names and metadata keys are sorted and checked for duplicates while the
frame evidence is created, making inspection and export deterministic.

```cpp
auto pixel = evidence->inspect({x, y});
if (const auto* normal = pixel->observation(WorldNormal{})) {
    // *normal is a Vec3; no variant inspection or cast is needed.
}
```

`RenderInvocationIdentity` is a structured set of opaque equality tokens. The
analysis core neither chooses a hash algorithm nor interprets their contents:
the renderer may store a stable serialization or a collision-resistant digest.
The four components identify the ordered workload, view, renderer definition,
and logical target contract. The renderer component includes neutral shader IR
and the invocation's effective graphics state, but excludes a raster variable deliberately
overridden by a diagnostic variant. The target component describes extent,
formats, and the readback contract rather than a transient native image handle.
This identity is optional for standalone evidence and mandatory when evidence
is placed in a `DiagnosticSweep`.

Metadata deliberately stores the backend-neutral camera snapshot, shader IR,
and linked interface description. Generated GLSL, source maps, pipeline state,
driver information, and logs belong in `BackendArtifact` instead of portable
`ShaderEvidence`. An artifact is owned text with a media type and a safe
relative name. Names may contain nested paths such as `shader/vertex.glsl`, are
sorted, and must be unique; absolute paths and `.`/`..` segments are rejected.
This keeps the analysis core independent of any particular backend while still
letting one evidence value carry everything needed to diagnose its production.

## Derived images

Coverage and identity images are derived from validated surface keys and do not
require more GPU attachments:

```cpp
auto coverage = make_coverage_visualization(capture);
auto items = make_item_visualization(capture);
auto primitives = make_primitive_visualization(capture);

// Or compute all three together.
auto visualizations = make_visualizations(capture);
```

Background is black, coverage is white, and item/primitive IDs use a fixed
integer hash to produce visible colors. The same keys always produce the same
bytes; the colors are labels, not material colors.

## Deterministic comparison

Exact comparison is the default. Frame IDs are ignored because they identify an
allocation of evidence rather than its visual contents:

```cpp
auto comparison = compare_captures(reference, actual);
if (!comparison->equivalent()) {
    std::cout << comparison->color_difference_count << '\n';
    std::cout << comparison->first_surface_key_difference->x << '\n';
}
```

Color and device-depth tolerances can be supplied explicitly. The result reports
counts, maxima, and first differing pixels in deterministic top-left row-major
order. It can also compare frame IDs and manifests, including provenance and
primitive-to-source topology. `compare_evidence` additionally checks the
request, metadata, additional-channel layout, and each additional value.
Backend artifacts are ignored by default because they may contain machine or
driver details; set `EvidenceComparisonOptions::compare_backend_artifacts` for
an exact deterministic artifact comparison.

## Counterfactual diagnostic sweep

When an ordinary frame is wrong, a renderer can run a small, fixed set of
counterfactual draws with the same tickets, camera, shader, extent, and
readback contract. `DiagnosticSweep` keeps the production evidence beside
those variants and derives conservative findings from their differences:

```cpp
auto sweep = DiagnosticSweep::create({
    {DiagnosticVariant::production, std::move(production)},
    {DiagnosticVariant::cull_disabled, std::move(no_cull)},
    {DiagnosticVariant::depth_always, std::move(always_depth)},
});

for (const auto& finding : sweep->findings()) {
    std::cout << finding.message << '\n';
}
```

The core does not toggle GPU state itself. A backend renders the named
variants and supplies their `FrameEvidence`; the analysis module validates
that production evidence exists, each variant is unique, all images have the
same extent, and every variant has the same complete `RenderInvocationIdentity`.
Frame IDs may differ because they identify separate readbacks, and ordinary
metadata such as `raster.variant` may differ because it records the tested
counterfactual. A workload, view, renderer, or logical-target fingerprint may
not differ: accepting one would let the sweep infer causality from unrelated
work. It then recognizes these evidence-backed cases:

- geometry visible only with culling disabled suggests winding or cull mode;
- geometry visible only with an always-pass depth test suggests depth clear,
  comparison, or projection convention;
- no coverage in any tested variant points toward draw counts, vertex input,
  transforms, or clipping rather than one of those raster states;
- surface coverage with only near-black covered pixels points toward material
  or fragment computation rather than missing geometry;
- non-finite covered depth and manifest items with no visible pixels are
  reported independently.

These are deliberately findings, not hidden corrective behavior. Production
state is never changed, and tools can inspect every underlying evidence bundle
before trusting a conclusion.

## Directory export

Export requires a nonexistent or empty directory so stale files can never look
like part of a newer capture:

```cpp
auto exported = export_evidence_directory(
    *evidence, "captures/frame-0042");
```

The stable version-1 bundle contains:

- `evidence.json`: request, portable metadata, invocation fingerprints,
  summary, full manifest and source primitive mapping, plus
  additional-channel descriptions;
- `summary.txt`: compact human/agent-readable totals;
- `pixels.csv`: exact RGBA, device depth, item, and primitive values in top-left
  row-major order;
- `channels.tsv`: exact values for any additional typed channels;
- `color.ppm` and `depth.pgm`: dependency-free directly viewable images;
- `coverage.ppm`, `items.ppm`, and `primitives.ppm` when requested by the
  diagnostic preset (or forced through `ExportOptions`);
- `artifacts/<name>` for every backend text artifact, preserving safe nested
  names such as `artifacts/shader/vertex.glsl`.

`evidence.json` records each artifact's name, media type, export path, and byte
count. Artifact text lives in its own file, so agents and ordinary tools can
open GLSL, logs, and pipeline descriptions directly without decoding JSON.

PPM drops alpha only for display; the exact alpha remains in `pixels.csv`.
Depth PGM clamps finite device depth to `[0, 1]`; exact values, including
non-finite markers, remain in the CSV. Numeric text uses locale-independent
round-trippable formatting, properties/channels are sorted, IDs are emitted in
manifest order, and the returned file list is sorted.

## Backend integration boundary

`OpenGLProgramRuntime::capture` performs the OpenGL integration. It is a
shader/diagnostic resource that a concrete renderer may own; it is not a
ticket renderer. Its public boundary is the backend-neutral request and result:

```cpp
auto evidence = shader_runtime.capture(
    device, cpu_mesh, gpu_mesh, view,
    CaptureRequest::diagnostic().observe(WorldNormal{}));
```

Internally, the runtime:

1. accepts a `CaptureRequest` rather than exposing attachment toggles;
2. produces the canonical `AnalysisCapture`;
3. attaches the camera snapshot and shader IR/interface as
   `FrameEvidenceMetadata`;
4. fingerprints the workload, view, program/runtime definition, and logical target in
   `FrameEvidenceMetadata::invocation`;
5. places newly emitted semantic images in canonical typed `EvidenceChannel`
   objects;
6. attaches generated source, source maps, pipeline descriptions, or driver
   reports as `BackendArtifact` values;
7. returns `FrameEvidence` and leaves summaries/inspection/visualization/export
   to this module.

`OpenGLProgramRuntime::diagnose` builds on the same boundary and supplies the
production, cull-disabled, and always-depth variants to `DiagnosticSweep`.
A concrete renderer exposes those operations only where they make sense for
its tickets, after validating that the diagnostic draw represents its ordinary
production policy. Neither `FrameEvidence` nor `DiagnosticSweep` depends on
OpenGL; another backend can produce the same portable results by following
these steps.
