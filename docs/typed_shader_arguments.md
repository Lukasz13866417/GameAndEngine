# Typed shader arguments and constants

A shader lambda receives symbolic DSL values; a draw call supplies ordinary
CPU values. The lambda executes once, while constructing backend-neutral IR.
Changing an argument does not rebuild the shader or replay its lambda.

## Small example

The direct mesh demo keeps the complete path visible in
[`examples/file_mesh_direct.cpp`](../examples/file_mesh_direct.cpp):

```cpp
namespace shader = vng::shader;
using vng::dsl::field;
using vng::dsl::Float;
using vng::dsl::Float4x4;

auto vertex = shader::vertex<VertexIn, VertexOut>(
    [](auto& s, Float4x4 model) {
        auto world = model * vng::dsl::vec4(s.input(Position{}), 1.0F);
        return s.output(
            field<shader::ClipPosition>(s.camera().project(world.xyz())),
            field<Color>(s.input(Color{})));
    });

auto fragment = shader::fragment<FragmentIn, FragmentOut>(
    [](auto& s, Float brightness) {
        auto color = s.input(Color{});
        return s.output(field<shader::Color<0>>(
            vng::dsl::vec4(color.xyz() * brightness, color.w())));
    });

// Check expected results before dereferencing; omitted here for brevity.
auto linked = shader::link(std::move(*vertex), std::move(*fragment));
auto program = vng::render::compile_program(device, *linked);

auto model = vng::Mat4::identity();
float brightness = 1.0F;
auto commands = frame.render_context();
commands.run(*program, model, brightness);
commands.view(view);
commands.draw(mesh);

brightness = 2.0F;
commands.run(*program, model, brightness);
commands.view(view);
commands.draw(mesh);
```

The input/output semantic interfaces and camera remain unchanged. Write
explicit DSL types for the lambda arguments after `auto& s`: `Float`, `Int`,
`UInt`, `Bool`, `Float2/3/4`, `Float3x3/4x4`, or `Expr<Record>`.
Ordinary `float`/`int` lambda arguments are rejected: arithmetic on them would
execute on the CPU instead of producing symbolic shader operations.

Arguments are positional: **vertex arguments, followed by fragment arguments**.
This example therefore produces `TypedGraphicsProgram<Mat4, f32>` and, for an
OpenGL device, `TypedProgram<Mat4, f32>`. Usually neither name needs spelling;
`auto` preserves the contract all the way to `run()`.

Submission checks exact logical types, ignoring reference/const qualifiers:

```cpp
commands.run(*program, model, 2.0F); // accepted
commands.run(*program, model, 2.0);  // rejected: double is not f32
commands.run(*program, 2.0F, model); // rejected: wrong order
commands.run(*program, model);      // rejected: missing argument
```

Values are copied during submission. Changing the CPU variable afterward has
no effect until the next `run()` or `set_arguments()` call. No borrowed pointers
to application values are retained. `run()` selects a shader and supplies its
arguments; `draw()` still performs the actual draw. It does not change depth,
culling, or blending policy. Camera-reading shaders still receive `view()`.

## Constant-only implicit operands

Expression arithmetic can lift a CPU operand only when it is a compile-time
constant of the correct type. This rule also applies to constructors and
mixed expression/constant helpers such as `min`, `mix`, and `select`.

```cpp
// Given an Int expression:
expression * 12;             // accepted
expression * (3 + 4);        // accepted
constexpr int fixed = 3;
expression * fixed;          // accepted

int changing = 3;
expression * changing;       // rejected
expression * (changing + 0); // rejected even though the operand is an rvalue
```

Internally a `consteval` conversion checks the CPU value. This is a
constant-expression restriction, not a textual-literal or rvalue restriction:
`constexpr` values and constant-evaluated helper results are also useful.
Signed/unsigned conversion and integer/float promotion remain explicit.

There are three deliberate choices:

- `expression * 0.5F`: an implicit compile-time constant.
- A `Float` lambda argument: a value supplied at draw time.
- `s.constant(cpu_value)`: explicitly bake the current CPU value into IR.

`s.constant()` also provides the builder when all operands are ordinary values.
An ordinary helper parameter loses its caller's constant-expression status.
Prefer helpers accepting expressions, for example
`Float brighten(Float value, Float amount)`, or explicitly bake values when
they genuinely configure shader construction, such as fixed filter taps.

Captures may still configure construction-time branches. They are not live GPU
parameters, and the compiler rejects implicit mixing of mutable captured
numeric values into expressions. Explicit `s.constant(captured)` remains an
intentional snapshot. Capturing configuration does not make arbitrary C++
control flow execute on the GPU.

## Semantic parameter records

Use an existing record schema to group related values without hand-written
uniform layouts or registration tables:

```cpp
struct Threshold : vng::gfx::Semantic<float> {};
struct Strength : vng::gfx::Semantic<float> {};
struct Exposure : vng::gfx::Semantic<float> {};
using Parameters = vng::gfx::Record<Threshold, Strength, Exposure>;

// In a shader lambda:
[](auto& s, vng::dsl::Expr<Parameters> parameters) {
    auto exposure = parameters.get(Exposure{});
    // ...return the complete output record...
};

// In ordinary rendering code:
Parameters parameters;
parameters.set(Threshold{}, 1.0F);
parameters.set(Strength{}, 0.25F);
parameters.set(Exposure{}, 1.2F);
commands.run(program, parameters);
```

The backend reads logical values through `get()`, not the record's encoded
bytes. A vertex codec therefore does not become a uniform/storage ABI.
Arbitrary C++ classes without a shader schema are not automatically reflected.

Separate stages do not share arguments just because their types happen to
match. To deliberately share their entire identical signature, use:

```cpp
auto linked = shader::link(std::move(*vertex), std::move(*fragment),
                           shader::shared_arguments);
```

Both stages must declare the same complete argument types in the same order;
the caller then supplies that list once. A common semantic record works well
here. Without the tag, the two lists remain independent and are concatenated.

## Backend lowering and diagnostics

The shader factory makes typed parameter-read expressions before invoking the
lambda. Their neutral IR declarations contain logical type and argument index,
not GPU handles. Linking resolves stage-local indices into the program call
signature. GLSL emission generates deterministic explicit-location uniforms;
record parameters are flattened into logical leaf uniforms, while matrices are
uploaded column-major. No native C++ aggregate layout is sent as a uniform block.

`render::compile_program(device, linked)` selects the backend through the
concrete device type and ADL. OpenGL uploads the supplied values through its
appropriate scalar/vector/matrix uniform operations. The typed facade carries
the call signature; the underlying validated IR and backend implementation stay
erased. There is no virtual dispatch per expression and no per-frame codegen.

Enhanced rendering keeps the same argument contract. The typed runtime's
`production()` returns a borrowed typed program view:

```cpp
commands.run(runtime.production(), model, eye);

// A capture can also be requested before any ordinary draw:
runtime.set_arguments(model, eye);
auto evidence = runtime.capture(device, cpu_mesh, gpu_mesh, view, request);
```

The runtime forwards the copied values to each diagnostic shader variant and
includes argument snapshots in capture identity. Keep the runtime alive and
unmoved while using its borrowed production view.

The migrations exercise different uses of the same mechanism:

- Direct cube: `Mat4 model` and `float brightness`.
- Renderer-owned cube: per-ticket brightness, including enhanced capture.
- Spaceship: `Mat4 model` and `Vec3 eye`, replacing a matrix-buffer workaround.
- Textured/glowing mesh: model, tint, emission; typed program providers preserve
  the contract across reload and replacement.
- Bloom: texel size plus a semantic parameter record; fullscreen geometry no
  longer changes per pass.
- Skinning: object and normal matrices; genuine indexed bone palettes remain
  buffer resources and are not repackaged as ordinary arguments.

Textures and indexed matrix buffers remain explicit resource-slot APIs. This
feature supplies typed values; it does not silently turn GPU resources into
copied scalar parameters.
