# Camera

`gfx::Camera` owns only backend-neutral pose and lens state. It has no window,
OpenGL handle, viewport, or GPU allocation. The render target supplies its
extent when a frame is rendered, so resizing cannot leave stale aspect-ratio
state inside the camera.

```cpp
vng::gfx::Camera camera;
camera.set_position({2.6F, 2.0F, 3.2F})
    .look_at({0.0F, 0.0F, 0.0F})
    .set_perspective({
        .vertical_fov = vng::degrees(50.0F),
        .near_plane = 0.1F,
        .far_plane = 100.0F,
    });
```

The default camera is valid: it sits at the origin, looks along `-Z`, uses
`+Y` as its up hint, and has a 60-degree perspective lens. `set_position`,
`move_by`, `look_at`, and `set_direction` change pose. `look_at` is an
orientation command: moving the camera afterwards preserves its direction; it
does not secretly keep tracking the old target.

An orthographic camera uses the same pose API:

```cpp
camera.set_orthographic({
    .vertical_height = 8.0F,
    .near_plane = 0.1F,
    .far_plane = 100.0F,
});
```

## Frame snapshot and conventions

For tools or a backend implementation, derive an immutable frame value
directly:

```cpp
auto snapshot = camera.snapshot({width, height});
if (!snapshot) {
    // snapshot.error() is a typed gfx::CameraDiagnostic.
}
```

The snapshot contains the normalized `right`, `up`, and `forward` basis, the
aspect ratio, and column-major `view`, `projection`, and `view_projection`
matrices. `view_projection` is `projection * view`, matching the shader DSL's
matrix-times-column-vector convention.

The engine's canonical camera convention is right-handed world/view space,
`+Y` up, camera-forward mapped to view-space `-Z`, and NDC depth `[-1, +1]`.
The current OpenGL backend uses that convention directly. A backend with a
different clip convention applies its correction at its own lowering/upload
boundary; game code and `Camera` do not acquire backend switches.

Snapshot construction reports zero-sized extents, non-finite state,
degenerate or parallel orientation vectors, invalid field of view, and invalid
near/far planes. Setters stay fluent and allocation-free; validation happens
at the existing fallible frame boundary.

## Shader and renderer use

The shader requests the renderer-provided camera parameter through typed,
backend-neutral IR:

```cpp
auto vertex = vng::shader::vertex<VertexIn, VertexOut>(
    [](auto& stage) {
        return stage.output(
            vng::dsl::field<vng::shader::ClipPosition>(
                stage.camera().project(stage.input(Position{}))),
            vng::dsl::field<Color>(stage.input(Color{})));
    });
```

There is no camera-specific vertex layout and no user-managed uniform
location. Application code snapshots the camera into the same extent as its
frame, then submits tickets to its concrete renderer:

```cpp
const auto extent = window.framebuffer_extent();
auto view = vng::render::RenderView::create(camera, extent);
auto frame = vng::render::begin_frame(device, frame_description);

auto drawn = renderer.render(*frame, *view, draws);
```

The IR records a read-only camera parameter. GLSL assigns an explicit uniform
location. An OpenGL renderer calls `commands.view(*view)` after binding a
pipeline; that uploads the snapshot before its draws. Moving or resizing the
camera therefore does not rebuild the IR, regenerate GLSL, or relink the
program. The enhanced analysis variant consumes the same camera parameter
automatically.
