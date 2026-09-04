# Graphics pipelines

A concrete renderer describes a compiled-program baseline with backend-neutral
shader IR and fixed-function state, then lets its selected device compile both:

```cpp
const vng::render::GraphicsPipelineDesc description{
    .depth = {
        .test = true,
        .write = true,
        .compare = vng::render::DepthCompare::less,
    },
    .cull = vng::render::CullMode::back,
    .front_face = vng::render::FrontFace::counter_clockwise,
    .output_encoding = vng::render::ColorEncoding::srgb,
};

auto pipeline = vng::render::compile_pipeline(
    device,
    shader_program,
    description);
```

`shader_program` is a linked `shader::GraphicsProgram`, not an OpenGL object.
`compile_pipeline` is a backend-neutral customization point whose result type
is inferred from `device`. For an OpenGL device the result is
`std::expected<vng::opengl::GraphicsPipeline,
vng::opengl::Diagnostic>`.

The OpenGL implementation performs the entire backend path once: deterministic
GLSL emission, driver shader compilation, program linking, and translation of
portable fixed-function state. Compilation and link errors retain the emitted
source, driver log, and IR-node source map. The resulting
`opengl::GraphicsPipeline` owns the linked program.
Successful pipelines retain the complete `glsl::ProgramSource` as well; it is
available through `generated_source()` for inspection and frame captures.
That interface metadata also tells `bind()` every fragment color-output
location it owns, including sparse MRT locations, so blend enable and color
write masks never leak in from an earlier pipeline.
Analysis and observation variants owned by `OpenGLProgramRuntime` retain the
complete interface metadata from their augmented emission too. They therefore
have the same exact sparse-output ownership as ordinary compiled pipelines even
though their GLSL product is compiled lazily.

The render-core header neither includes nor enumerates OpenGL backends. A
second device backend supplies its own `compile_graphics_pipeline` overload
without changing the neutral API.

## Defaults

The default description is deliberately inert: depth testing and depth writes
are both disabled, culling is disabled, front faces are counter-clockwise, and
presentation output is sRGB. That encoding matches the default
`opengl::ContextDesc` and `FrameDesc`; linear off-screen and analysis targets
select `ColorEncoding::linear` explicitly. Enabling depth writes without depth
testing has surprising and backend-dependent practical behavior, so callers
opt into both explicitly.

`ColorEncoding` is a render-target contract, not an OpenGL enum in the
portable API. OpenGL validates the attachment's physical encoding and realizes
`srgb` writes with framebuffer conversion; a backend whose encoding lives in
an attachment or image-view format can validate or select that format while
compiling the same request.

## Low-level escape hatch

Backend integration tests, importers for precompiled backend shaders, and
specialized tooling may already own a backend program. That narrower operation
is named as realization and placed behind the expert namespace:

```cpp
auto pipeline = vng::render::expert::realize_pipeline(
    device,
    std::move(opengl_program),
    description);
```

OpenGL validates the complete description and device identity before moving
from the program. A failed realization therefore leaves the supplied program
owned by the caller. This API is not the ordinary application path.
Because a raw program has no neutral interface metadata, OpenGL uses a
conservative fallback when binding an expert pipeline: it disables blending
and enables full color writes for every supported draw-buffer slot. The expert
path therefore remains deterministic without making realization require a
current context.

## Renderer-controlled binding and drawing

An OpenGL renderer uses the command stream belonging to its active frame:

```cpp
auto commands = frame.commands();
commands.bind(*pipeline);
commands.view(view);
commands.draw(gpu_mesh);
```

The renderer is free to bind another pipeline or call `state`, `depth`, and
`cull` between draws. `bind` establishes the compiled baseline; subsequent
fine-grained changes are renderer policy, not a new renderer object.

At draw time the mesh derives and caches a VAO from the immutable
semantic/location contract carried by the bound pipeline. Direct
`GpuMesh::draw(device, pipeline)` and `draw_bound` calls remain backend-level
escape hatches for integration code and tests. Raw-program overloads remain
expert paths for low-level backend tooling.

Pipeline descriptions do not contain cameras, render targets, clears, meshes,
or per-object values. Those belong to frame setup, concrete renderer resources,
and render tickets. A renderer may own several descriptions/pipelines and use
them in whatever order its policy requires.
