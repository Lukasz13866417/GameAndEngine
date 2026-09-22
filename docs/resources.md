# Resource providers and owners

A provider retains the information needed to obtain a resource. An owner
retains the resulting resource and decides which dependencies must change
together. Builders collect providers and create ready owners; ordinary RAII
releases the owned resources.

The examples below assume a current `opengl::Device& device`. They omit most
error handling for readability. Provider-backed builders and update methods
return `resources::Result<T>`, an `std::expected<T, resources::Diagnostic>`.
Existing lower-level graphics factories and rendering APIs can still return
their original backend diagnostics.

## Build and reuse

```cpp
#include <vng/resources_opengl/mesh_renderer.hpp>
#include <vng/providers/image.hpp>
#include <vng/providers/mesh.hpp>

auto builder = vng::render::mesh_renderer_builder(device);
builder.mesh(vng::providers::mesh(cpu_mesh));
builder.albedo(vng::providers::texture_file("assets/albedo.png"));

auto first = builder.build();
auto second = builder.build();
```

Both renderers retain owning handles to their reconstruction recipes, so the
builder and original provider variables may disappear. Builds normally create
independent GPU programs, images and geometry. They do not implicitly share a
GPU texture just because they use the same provider.

Builders borrow the `Device` facade passed to them. Keep that object alive and
unmoved until all `build()` calls are complete. A mesh `Update` similarly borrows
its device until `commit()`. The underlying context must remain alive and be
current on its owner thread for GPU operations and destruction, as with the
existing OpenGL resource types.

The mesh renderer supplies its own DSL program and a white fallback albedo.
It is an unlit textured/emissive renderer: its canonical vertex fields are
`gfx::Position` (`Vec3`), `gfx::TexCoord<>` (`Vec2`) and `gfx::Color` (`Vec4`).
It does not provide material lighting, normals, PBR or transparency sorting.
`render::SurfaceDraw` supplies a transform, tint, emission and depth/cull choices.

```cpp
vng::render::SurfaceDraw draw{
    .tint = {0.2F, 0.8F, 1.0F, 1.0F},
    .emission = 7.0F,
};
auto rendered = first->render(frame, view, draw);
```

Emission multiplies the sampled, vertex-tinted linear RGB by `1 + emission`.
Render into a floating-point target to preserve values above one for
[bloom](bloom.md).

Pass a span of `SurfaceDraw` tickets to batch instances. Adjacent tickets with
the same depth-test, depth-write and culling choices produce one native
instanced draw, even with different transforms, tints and emission. The
renderer retains its instance buffer capacity. It preserves submission order:
state sequence A/A/B/A produces three runs, not reordered A/A/A and B.
`renderer.stats()` reports the latest submission's `instances` and `draw_calls`.
Invalid ticket values are rejected before any of that submission draws.

## Providers are ordinary values

A custom provider only needs a repeatable `const provide(...)` operation:

```cpp
#include <vng/gfx/buffer.hpp>
#include <vng/opengl/resources.hpp>
#include <vng/resources/resources.hpp>

struct BufferProvider {
    std::vector<std::byte> bytes;

    auto provide(vng::opengl::Device& device) const {
        return vng::gfx::make_buffer(device, bytes);
    }
};

auto from_function = vng::resources::provider(
    [bytes = std::move(bytes)](vng::opengl::Device& device) {
        return vng::gfx::make_buffer(device, bytes);
    });
```

The common protocol accepts `provide()`, `provide(context)`, and
`provide(context, request)`, returning an expected resource. The
`resources::provide` dispatcher can omit an unused context, including calling
`provide(request)` for a CPU provider. It never silently drops a request.
Calling a provider must not consume the recipe or move the same product out
repeatedly. A file provider can observe changed file contents on a later call.

`resources::Provider<Resource, Context, Request>` is the optional owning,
type-erased form. Its context and request default to `void`. Copies share the
immutable recipe; the dispatch cost occurs only during provisioning. Normal
resource access and drawing do not traverse provider type erasure. Custom
providers need no base class or registration.

`resources::into_result(...)` converts an expected backend result and preserves
its error. `Diagnostic::cause_as<BackendDiagnostic>()` exposes the retained
original diagnostic, including backend-specific codes or source evidence.
Provider failures can also carry a message, context notes, driver log and
generated source. Inspect the result before dereferencing it.

## Reload, replace and commit

For the mesh owner, `mesh`, `program` and `albedo` are separate replacement
boundaries:

| Operation | Resource and recipe installed on success |
| --- | --- |
| `reload_albedo(device)` | A newly provided image and the existing provider. |
| `reload_albedo(device, provider)` | A newly provided image and the new provider. |
| `replace_albedo(device, image)` | The ready image; its previous provider is removed. |
| `replace_albedo(device, resources::provided(image, provider))` | The ready image and its provider. |

The same vocabulary applies to `mesh` and `program`. Source-less targeted
reload returns `ErrorCode::no_provider`. Recursive `reload(device)` refreshes
configured providers, keeps manually supplied inputs, and returns a
`ReloadReport` with `refreshed` and `retained` names. Refreshing another input
may rebuild a dependency derived from a retained input.

```cpp
auto updated_image = first->reload_albedo(
    device, vng::providers::texture_file("assets/winter.png"));

auto pending = first->update(device);
pending.mesh(vng::providers::mesh(new_cpu_mesh));
pending.program(vng::providers::program(new_shader_ir));
auto committed = pending.commit();
```

Include `<vng/providers/program.hpp>` for the program provider. The grouped
update prepares candidates and validates the mesh/program combination before
installing any of them. A failure preserves the committed resources and their
providers. Abandoning an update discards pending work. Updates are tied to the
owner revision; another successful update makes an older transaction stale.

Resource replacements, recursive reloads, target resizes and provider-backed
builds require a boundary outside any active `Frame` on that context. End the
frame and discard its resource borrows first. These operations refresh a
functioning device; they are not a device-loss recovery mechanism. Transactions
are per owner, not a scene-wide atomic update across several owners.

Custom mesh programs use ordinary DSL IR with the instance contract from
`<vng/render/mesh_renderer.hpp>`. In addition to any supported mesh attributes,
their vertex inputs must declare all of these semantic tags:

```cpp
using namespace vng;
using namespace render::surface;
using Inputs = shader::VertexInputs<
    gfx::Position, gfx::TexCoord<>, gfx::Color,
    ModelColumn<0>, ModelColumn<1>, ModelColumn<2>, ModelColumn<3>, Tint, Emission>;

// Inside the vertex shader:
auto model = dsl::make<Mat4>(
    stage.input(ModelColumn<0>{}), stage.input(ModelColumn<1>{}),
    stage.input(ModelColumn<2>{}), stage.input(ModelColumn<3>{}));
```

`ModelColumn` and `Tint` are `Vec4`; `Emission` is `f32`. The renderer fills
`render::surface::Instance` records and binds them with divisor one. To use
tint/emission in the fragment shader, forward them through flat varyings.
Custom programs may omit unused mesh attributes, but cannot require other
vertex semantics. `opengl::SurfaceProgram` is an ordinary `Program`; the
builder/replacement boundary validates its semantic metadata before committing.
Uniform-only `(model, tint, emission)` shaders are rejected explicitly, not
silently drawn once per ticket or incorrectly instanced.

This owner supplies the camera and texture slot 0 and expects one `vec4` color
output at location 0. It does not supply explicit shader arguments or
matrix-buffer resources. Providers and ready replacements retain the same
transactional API; incompatible programs leave resources and recipes intact.

## Transfer a ready resource

```cpp
auto provider = vng::resources::share_provider(
    vng::providers::texture_file("assets/albedo.png"));
auto ready = provider.provide(device);

auto installed = first->replace_albedo(device,
    vng::resources::provided(std::move(*ready), provider));
```

`provided()` packages a value and its recipe. It does not invoke the provider.
Adopting an already-created `Image2D` or `Program` preserves that GPU resource;
there is no second image upload or program compilation. The owner still
validates compatibility and can allocate small C++ ownership handles. The
package has no independent reload operation that could bypass the owner.

Meshes have a different concrete resource boundary. `providers::mesh(...)`
captures a canonical CPU snapshot, retaining topology and source information.
Custom field tags can be mapped with `render::MeshFields<Position, UV, Color>`;
missing UVs default to zero and missing vertex color to white. The provider
produces `render::MeshSource`, a shared CPU source, not an uploaded `GpuMesh`.
`replace_mesh(device, ready_source)` snapshots and uploads that source and
validates its shader input contract. The inferred mesh/instance VAO is cached
on first submission. It does not adopt an arbitrary existing GPU
mesh without uploading. The committed CPU snapshot stays paired with the
geometry actually uploaded, even if the caller later edits its original mesh.

## Share recipes or products explicitly

```cpp
auto recipe = vng::resources::share_provider(
    vng::providers::texture_file("assets/shared.png"));
left_builder.albedo(recipe);
right_builder.albedo(recipe);

auto image = recipe.provide(device);
auto shared_image = vng::resources::share_resource(std::move(*image));
auto left_result = left->replace_albedo(device, shared_image);
auto right_result = right->replace_albedo(device, shared_image);
```

`share_provider()` shares reconstruction information. `share_resource()`
creates a shared owning handle to a stable product. Replacing one consumer's
image does not automatically replace another consumer's image. There is no
live asset registry, file watcher or implicit scene-wide reload propagation.
Shared image handles and ready-image packages both work with `replace_albedo`.

Texture providers accept owned RGBA8 `gfx::ImageData` or a PNG/binary P6 PPM
file. Their default upload format is `srgb8_alpha8`, with linear filtering and
a generated mip chain. Use an appropriate linear format for linear data.
`gfx::SamplerDesc` specifies minification, magnification, mip filtering and
wrapping; the current backend stores those settings on the image. There is
no public separate sampler resource. Decoded row zero is the top image row;
UV authoring controls image orientation.

## Fonts and skins

```cpp
#include <vng/text_opengl/text_renderer.hpp>
#include <vng/text/font_provider.hpp>

auto text_builder = vng::render::text_renderer_builder(device);
text_builder.font(vng::providers::font_file("assets/ui.ttf"));
auto text = text_builder.build();
auto changed_font = text->reload_font(device);
```

Text font replacement invalidates font-dependent glyph, layout and atlas
caches together. `replace_font(device, font)` clears the old font provider;
`replace_font(device, resources::provided(font, provider))` retains a new one.
`reload(device)` reconstructs the program and caches and refreshes the default
font when it has a provider. A font supplied on an individual text ticket
continues to belong to that ticket's caller. See [text rendering](text_rendering.md).

```cpp
#include <vng/rig_opengl/skinned_mesh_renderer.hpp>
#include <vng/rig/skin_provider.hpp>

auto skin_builder = vng::render::skinned_mesh_renderer_builder(device);
skin_builder.skin(vng::providers::skin(binding));
auto skinned = skin_builder.build();
auto changed_skin = skinned->reload_skin(
    device, vng::providers::skin(updated_binding));
```

The skin boundary includes mesh, weights and armature association. The owner
validates and rebuilds them together. `replace_skin` accepts a ready binding,
with or without a provider package. Animation poses remain external state;
reload does not reset them. A pose must still be compatible with the newly
installed binding. See [rigging](rigging.md).

## Dependencies and examples

`vng::resources` is a header-only core facility with no OpenGL, window, shader,
font or image-decoder dependency. `vng::providers` bundles the neutral content
and shader provider helpers. Texture file decoding belongs to `vng::content`
and uses libpng; PNG development files must be available at configure time.

The concrete mesh owner is in `vng::resources_opengl`, bloom in
`vng::bloom_opengl`, text in the optional `vng::text_opengl`, and skinning in
`vng::rig_opengl`. Each concrete OpenGL layer owns its native allocations.
Their device-selected facade headers do not pull GLFW into the engine layers.
The glow demo links the window/context integration separately and works with
`VNG_BUILD_TEXT=OFF` (without its text HUD).

Run `vng_glow_demo` for mesh/image providers, retained HDR targets, resize,
resource reload and bloom in one application. [Bloom and HDR targets](bloom.md)
shows the frame sequence and demo controls.
