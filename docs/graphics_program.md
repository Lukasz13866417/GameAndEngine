# Shader programs and draw-state ownership

A compiled program owns shader executables and their interface metadata.
It does not own depth, culling, blending, polygon mode, or target encoding.

```cpp
auto program = vng::render::compile_program(device, shader_program);
auto context = frame.render_context();
auto graphics = context.graphics_state();

graphics.set(vng::render::DepthTest{true});
graphics.set(vng::render::DepthWrite{true});
graphics.set(vng::render::CullMode::back);
context.run(*program);
context.view(view);
context.draw(gpu_mesh);
```

Snippets omit error checks; compilation and command operations return
`std::expected` results.

## Compilation boundary

`shader_program` is linked backend-neutral shader IR. The `compile_program`
customization point dispatches through the concrete device without making
render-core headers enumerate or include graphics backends. OpenGL returns
`std::expected<opengl::Program, opengl::Diagnostic>`.

OpenGL emits deterministic GLSL, compiles both stages and links them once.
A successful program retains the complete `glsl::ProgramSource` through
`generated_source()`: source text, semantic interfaces, shader-argument
metadata and source maps. Failures retain generated source, driver logs and
IR-node mappings where available. Enhanced diagnostic shaders can use the
same compiler overload with an already emitted `glsl::ProgramSource`.

Raw backend integration can compile `opengl::Shader` values and call
`Program::link_graphics` directly. No additional wrapper or realization step
is required. Metadata absent from raw shader sources cannot be inferred by
the engine; callers must provide the contracts needed by their chosen path.

## State boundary

The frame owns one live command context. Selecting another program leaves
graphics settings unchanged. `run(program)` avoids redundant native selection;
`bind(program)` explicitly rebinds. Both select shaders, while `draw` issues
the actual rendering.

A renderer chooses settings when drawing, not when creating its program.
Nested renderers share this context and establish their own required state;
there is no hidden preset or implicit push/pop. Callers can explicitly save and
restore managed settings with `graphics.snapshot()` and `graphics.set(snapshot)`.
These snapshots are values, not shader resources.

Frame setup owns render-target encoding, clears and viewport. It validates
physical attachment encoding independently of which program will draw.
GPU meshes resolve and cache vertex-input bindings from program semantics.

See [live graphics state](graphics_state.md) for defaults, backend extensions
and lifetime rules, and [renderers](renderer_api.md) for composition and capture.
