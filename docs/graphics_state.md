# Programs and per-draw graphics state

Choose state where a draw needs it. No persistent settings object or pipeline
is required. Snippets omit checking `std::expected` results; the examples check
every operation.

```cpp
auto program = vng::render::compile_program(device, shader_program);
// ... begin a frame ...
auto context = frame.render_context();
auto graphics = context.graphics_state();

graphics.set(vng::render::DepthTest{true});
graphics.set(vng::render::DepthWrite{true});
context.run(*program);
context.view(view); // when the shader reads a camera
context.draw(world_mesh);

graphics.set(vng::render::DepthTest{false});
context.draw(overlay_mesh);

graphics.set(vng::render::DepthTest{true});
context.draw(other_mesh);
```

Each setting remains in effect until changed. Changes affect subsequent draws
only. `run(program)` selects shaders; execution happens on `draw`. Switching
programs preserves graphics settings. Selecting the same program again skips
the native bind and preserves its uploaded view. Switching programs requires
a view upload for the newly selected program if it uses camera parameters.

`compile_program(device, ir)` returns the backend's compiled `Program` through
static dispatch. OpenGL programs own executables and generated GLSL/interface
metadata, including camera parameters, independently of depth or culling.
GPU meshes infer VAOs from program semantics; `prepare_vertex_input(device,
program)` optionally prewarms them.

## Settings and defaults

Portable setters accept `render::DepthTest`, `DepthWrite`, `DepthCompare`,
`CullMode`, `FrontFace`, and `BlendMode`. `DepthState` is an optional grouped depth update.

```cpp
graphics.set(vng::render::DepthWrite{false});
graphics.set(vng::render::DepthCompare::less_equal);
graphics.set(vng::render::CullMode::back);
graphics.set(vng::render::FrontFace::counter_clockwise);
graphics.set(vng::render::BlendMode::premultiplied_alpha);
```

Each setter preserves other choices. OpenGL does not write depth while testing
is disabled, even if its write flag is enabled. Use `DepthCompare::always` with
testing and writing enabled to write depth without rejecting by depth.

`BlendMode` affects color attachment 0. Choose `straight_alpha` for ordinary
RGB plus opacity, or `premultiplied_alpha` when shader RGB already includes
opacity. Both compose alpha with source-over semantics. `disabled` replaces
the destination. The [text renderer](text_rendering.md) selects premultiplied
blending internally; callers do not need to configure it.

New frames default to testing off, writing off, comparison `less`,
no culling, disabled blending, counter-clockwise front faces, and filled polygons. Defaults are
established lazily on the first valid setter, program selection, or draw,
independently of ambient GL state. Output encoding belongs to the frame target.

## Backend dispatch

`render::GraphicsState<BackendState>` forwards `set` to concrete backend overloads.
Applications obtain it through `auto`. Concepts expose compatibility:

```cpp
static_assert(vng::render::SupportsGraphicsSetting<
    vng::opengl::GraphicsState, vng::render::DepthTest>);

// An OpenGL extension to the portable setting vocabulary:
graphics.set(vng::opengl::PolygonMode::line);
```

Unsupported types have no matching overload. Portable headers do not enumerate
backends. General renderers use portable types; specialized renderers can also
use their backend's settings. There is no virtual dispatch or per-setting
allocation. OpenGL skips unchanged setting groups. A future backend can retain
desired values for native pipeline selection at draw time; no other graphics
backend is implemented here. Device capabilities still need runtime validation.

## Lifetime and raw GL

Every command and graphics-state handle from a frame borrows the same live
program, view readiness and desired graphics state. Acquiring, moving or dropping
a command handle does not reset state or revoke sibling handles. Moving the frame
transfers that same scope; only ending or destroying its owner revokes the handles.
Handles retain context storage and the frame generation, never pointers to C++
Frame or Commands objects. Expired, wrong-thread and non-current-context calls fail
before changing GL state.
Acquiring a graphics-state handle from a temporary command handle, or binding a
temporary program, is rejected at compile time.

A parent renderer can keep its context across child renderer calls. There is no
implicit push/pop of state: a child establishes its requirements, and the parent
selects its program and required settings again before continuing. Calling
`frame.render_context()` again is an inexpensive borrow, not a new state boundary.

`context.bind(program)` forces a program bind while preserving graphics state.
After raw GL changes state or the program, use `context.restore()` and upload
the view again. This reasserts desired graphics state, program and frame encoding.
It does not restore a raw-GL change of framebuffer or viewport; the caller must
restore those frame-owned bindings. Repeated setters assume exclusive control
of the GL state.

Explicit `RenderStateScope` users such as bloom restore native state and invalidate
managed program/view and graphics caches on exit. Existing handles remain valid;
reselect the program, upload its view if needed, and set the next draw's state.
These scopes deliberately do not restore a hidden snapshot of the managed context.

## Explicit snapshots and diagnostic capture

`graphics.snapshot()` returns an expected backend-specific value containing
the current managed settings; it does not read ambient raw GL. OpenGL's
`GraphicsStateSnapshot` contains depth test/write/compare, culling, winding,
blend mode, and polygon mode. It contains neither shaders nor target encoding.

```cpp
auto saved = graphics.snapshot();
if (!saved) return std::unexpected(saved.error());
// ... child rendering changes the live context ...
graphics.set(*saved);
context.run(program);
context.view(view);
```

Restoring a snapshot validates every setting before applying any of them.
It does not restore program selection, camera readiness, framebuffer, or viewport.
Snapshot access has the same frame/context/thread validation as setters.
When raw GL has changed tracked state, reconcile it through `context.restore()`
before treating a managed snapshot as evidence of the actual draw.

The diagnostic runtime owns programs, not retained draw-state defaults.
`runtime.capture(frame, ...)` and `runtime.diagnose(frame, ...)` snapshot this
invocation's managed settings and the frame encoding automatically. A low-level
Device-based capture requires an explicit `opengl::CaptureState{raster, encoding}`.
Unsupported capture configurations fail rather than quietly substituting
shader-creation defaults.

See [the direct mesh example](../examples/file_mesh_direct.cpp) and
[the custom renderer](../examples/file_mesh_renderer.cpp). Tests compare rendered
pixels while depth testing/writing change between draws and cover program
switches, backend settings, and revoked handles.
