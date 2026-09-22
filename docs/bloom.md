# HDR targets and bloom

`render::bloom_builder(device)` creates a ready effect that owns its target
pyramid, fullscreen geometry and fixed DSL programs. Its providers retain
reconstruction choices. The application supplies a rendered linear HDR image
and an active output frame; changing bloom strength, exposure or threshold
does not rebuild or recompile resources.

## Render an HDR scene

```cpp
#include <vng/bloom_opengl/bloom.hpp>
#include <vng/opengl/render_target.hpp>
#include <vng/providers/target.hpp>

const vng::Extent2D extent{1280, 720};
auto target_provider = vng::providers::render_target({
    .color = vng::gfx::ImageFormat::rgba16f,
    .depth = true,
});
auto scene = target_provider.provide(device, extent);

auto builder = vng::render::bloom_builder(device);
builder.levels(5);
auto bloom = builder.build(extent);
```

The snippets omit error handling; check each expected result in real code.
`RenderTarget` owns its color image, optional depth image, framebuffer and
provider. The original `target_provider` and bloom builder may disappear after
creation. `render::make_target(device, TargetDesc, extent)` also directly creates
the same ready target. The target's `color()`, `depth()` and `framebuffer()`
accessors borrow its current resources.

Render to the HDR target first, end that frame, then composite into the output:

```cpp
auto scene_frame = vng::render::begin_frame(device, *scene, {
    .extent = extent,
    .color_encoding = vng::render::ColorEncoding::linear,
    .clear_color = std::array<float, 4>{0.003F, 0.005F, 0.01F, 1.0F},
    .clear_depth = 1.0F,
});
auto drawn = renderer.render(*scene_frame, view, draws);
auto scene_ended = scene_frame->end();

// Select the encoding matching the physical default framebuffer created by
// your context integration. This example assumes an sRGB default framebuffer.
auto output = vng::render::begin_frame(device, {
    .extent = extent,
    .color_encoding = vng::render::ColorEncoding::srgb,
});
auto composited = bloom->apply(*output, scene->color(), {
    .threshold = 1.0F,
    .strength = 0.25F,
    .exposure = 1.0F,
});
// Optional text/UI can be drawn into this output frame after composition.
auto output_ended = output->end();
```

The OpenGL context permits one active `Frame` at a time. A scene frame and an
output frame are consecutive scopes, not nested scopes. The effect's internal
passes use private framebuffer bindings inside the output scope and restore
the caller's target on return. Presentation remains the window's responsibility.

An offscreen frame's extent and color encoding must match its physical
attachments. Request a depth clear only when the target has a depth attachment.
Existing `opengl::Framebuffer` values also work with
`render::begin_frame(device, framebuffer, description)`.

## Settings and image contract

`render::BloomSettings` has three finite, nonnegative values:

| Field | Default | Meaning |
| --- | --- | --- |
| `threshold` | `1.0` | Maximum linear RGB component above which pixels contribute to bloom. |
| `strength` | `0.25` | Scales the filtered halo added to the original scene. |
| `exposure` | `1.0` | Scales combined linear radiance before tone mapping. |

Input must be a same-device, single-sample `RGBA16F` or `RGBA32F` image whose
extent matches both the effect and the output frame. The source must not be
attached to the output frame: reading and writing the same attachment would
create texture feedback and is rejected.

The effect writes normalized or floating color output 0. It preserves source
alpha, additional color attachments and depth; integer or missing output 0 is
rejected. Fixed-function blending is disabled during composition because this
is a complete image replacement. A typical scene clears HDR alpha to one.
Bloom does not implement a transparent-scene compositing convention merely by
preserving the source alpha channel.

The fixed shaders threshold source samples, build a filtered half-resolution
pyramid, progressively upsample it, and combine it with the original HDR image.
Each pixel's exposed RGB is tone mapped with `x / (1 + x)` (Reinhard). The output
frame controls final hardware sRGB encoding. Do not pre-encode the HDR input or
apply a second manual gamma conversion for an sRGB output frame.

`strength = 0` skips pyramid draws while retaining exposure and tone mapping.
`exposure = 0` produces black RGB. Raising the threshold above scene brightness
removes the halo. Bright source values above one are necessary for the default
threshold; an ordinary clamped RGBA8 scene cannot retain that radiance.

The built-in mesh renderer's emission produces bright linear color for this
effect. It is unlit and does not calculate physically based illumination,
light transport, shadows or surface reflections.

## Resize and reload

```cpp
// Outside every active Frame; skip rendering while the window extent is zero.
auto resized_scene = scene->resize(device, new_extent);
auto resized_bloom = bloom->resize(device, new_extent);

// Recreate from retained recipes at the current committed extent.
auto reloaded_scene = scene->reload(device);
auto reloaded_bloom = bloom->reload(device);
```

Each owner stages and validates a complete replacement before committing it.
Failure keeps that owner's previous resources and extent. These are separate
transactions: check both resize results, and do not render until scene, effect
and output extents agree. No zero-size target is allocated for a minimized
window. Reacquire resource borrows after a successful replacement.

Bloom `resize()` replaces target images and framebuffers while keeping its
programs. `reload()` rebuilds the programs, fullscreen geometry and targets.
Settings passed to `apply()` remain application state. The provider's configured
level count is in `[1, 8]`; allocation stops when both dimensions reach one.
`levels()` reports the actual allocated count, which can therefore decrease
after a resize. Every allocated level uses floating-point RGBA16F storage and
linear filtering. The filter programs are authored through the engine DSL;
there is no per-frame generated shader source. Fullscreen geometry is immutable;
the texel size and a semantic `Record<Threshold, Strength, Exposure>` are typed
fragment-shader arguments. They are uploaded once per pass, not duplicated in
each vertex or sent through artificial varyings.

`apply()` restores framebuffer, viewport, raster, depth, indexed color-write,
blend, program and vertex-array state. It also restores its borrowed 2D texture
and sampler bindings on units 0 and 1. Existing frame command handles remain
valid. The explicit native-state scope invalidates managed binding/view and
graphics caches, so reselect the next program, upload its view if needed and
establish its required graphics settings through the same handle. No new frame
or command context is needed. Capturing this state has an
explicit per-call cost; this effect does not rely on an implicit global pass
graph or persistent render-state cache.

Bloom is a display post-process. Its RGB output has been blurred and tone
mapped; it is not a source-surface identifier, world-position observation or
linear HDR analysis capture. Existing analysis APIs retain their own evidence
and attachment semantics. Additional attachments remain untouched by bloom,
but their values are not transformed into new bloom-aware evidence.

## Demo

```sh
cmake --build build --target vng_glow_demo
./build/vng_glow_demo
./build/vng_glow_demo --once
./build/vng_glow_demo --frames 120 --reload
./build/vng_glow_demo --no-bloom
```

`vng_glow_demo` combines procedural mesh and image providers, emissive draws,
a retained HDR render-target provider and bloom. `B` toggles bloom, `Space`
pauses animation, `R` requests resource reload, and `Esc` closes the window.
`--once` and `--frames N` bound the run; `--no-bloom` starts with bloom disabled,
and `--reload` exercises resource reconstruction during the run.

The demo requires OpenGL and GLFW. When `VNG_BUILD_TEXT` is enabled it adds a
text HUD; the scene and effect also build without the font dependencies.
Link `vng::bloom_opengl` for the effect and `vng::resources_opengl` for the
provider-backed mesh renderer. See [resources](resources.md) for the ownership
contracts and the distinction between sharing a provider and sharing a GPU image.
