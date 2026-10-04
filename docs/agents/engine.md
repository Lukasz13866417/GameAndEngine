# Engine libraries (`include/vng`, `src`)

Read this when you change anything under `include/vng/` or `src/`, add or relink an
engine target, or need to know which library owns a concept. The editor application
(`examples/editor`) is covered in [editor.md](editor.md), and scene generators in
[scenes-and-cinematics.md](scenes-and-cinematics.md). Numeric limits are listed in
[limits-and-non-features.md](limits-and-non-features.md). Known-failing tests and other
volatile facts are in [status.md](status.md).

**TL;DR**
- The `moduleTargets` table in [`tests/layering/layering.test.cjs`](../../tests/layering/layering.test.cjs) maps each directory to a target (not by name). Run the layering test after any include or link change.
- Neutral contracts reach OpenGL only through CPOs plus ADL, with no virtuals and no registry. Backend types live in `vng::opengl`.
- Recoverable errors return `std::expected<T, <module>::Diagnostic>`, and broken preconditions throw. API misuse is a compile error, pinned by `tests/compile_fail`.
- Shaders are a C++ DSL: the lambda runs once, then IR → GLSL 4.60 → GL. The engine and examples have no hand-written GLSL (only tests pass hand-written source to `opengl::Shader::compile`; `examples/advanced_instanced_streams.cpp` feeds it DSL-generated GLSL), no compute stage and no control flow.
- There is no `-Werror`, so read the compiler output yourself. Match the style of the file you edit.

## 1. Library map

- **Kind:**
  - INTERFACE means header-only. These targets get no warning flags of their own; their
    headers are only checked when a compiled target includes them.
  - *glob* means a new `src/<dir>/*.cpp` file is picked up automatically.
  - *list* means you must add the `.cpp` file to `CMakeLists.txt` by hand.
- **Option gates** are shown in capitals, e.g. TEXT means `VNG_BUILD_TEXT`. A gated target exists only when all of its gates are ON. See [build-and-test.md](build-and-test.md).

| Dir (`include/vng/…`, `src/…`) | Target | Kind | Public links | Owns | Read |
|---|---|---|---|---|---|
| `core`, `input` | `vng_core` | INTERFACE | — | `vng::f32`, `Vec3`, `Mat4`, `Extent2D`, `degrees`, `monotonic_ns()`, `core::type_name<T>()`; `vng::input` keys, events, `Clipboard`, and event routing (`EventSequence`, `AvailableEvents` in `input/routing.hpp`) | [codebase.md](../codebase.md) |
| `timeline` | `vng_timeline` | STATIC, list | core | Keyframed property tracks | [timeline.md](../timeline.md) |
| `spatial` | `vng_spatial` | STATIC, list | core | `TriangleBvh` ray picking | — |
| `resources` | `vng_resources` | INTERFACE | core | `Result`, `Diagnostic`, `DiagnosticCause`, provider concepts, `Shared<T>`, `Provided<T,P>` | [resources.md](../resources.md) |
| `gfx` | `vng_gfx` | INTERFACE | core | Semantics, `Record`, codecs, vertex streams and layouts, CPU `Mesh`, `Camera`, image and buffer CPOs | [shader_vertex_pipeline.md](../shader_vertex_pipeline.md), [camera.md](../camera.md), [mesh_and_vmesh.md](../mesh_and_vmesh.md) |
| `analysis` | `vng_analysis` | INTERFACE | gfx | Frame evidence, comparison, diagnosis sweeps, export | [analysis_evidence.md](../analysis_evidence.md) |
| `render` | `vng_render` | INTERFACE | gfx, resources | `Renderer<Ticket,Backend>`, `begin_frame`, `compile_program`, `make_target`, `GraphicsState`, `RenderView`, `DepthMapping` | [renderer_api.md](../renderer_api.md), [graphics_state.md](../graphics_state.md) |
| `shader` (+ `dsl/`) | `vng_shader` | STATIC, glob | gfx | The DSL (`vng::dsl`), IR, validate/optimize, `link`, typed arguments | [shader_vertex_pipeline.md](../shader_vertex_pipeline.md), [typed_shader_arguments.md](../typed_shader_arguments.md) |
| `glsl` | `vng_glsl` | STATIC, glob | shader | Deterministic GLSL 4.60 emitter, source maps, analysis variants | [shader_vertex_pipeline.md](../shader_vertex_pipeline.md) |
| `content` | `vng_content` | STATIC, glob | gfx (PNG private) | `Document` parser/writer, `.vmesh` and its schema, PNG/PPM `load_image` | [documents.md](../documents.md), [mesh_and_vmesh.md](../mesh_and_vmesh.md) |
| `providers` | `vng_providers` | INTERFACE | resources, render, shader, content | Image, mesh, program and render-target providers | [resources.md](../resources.md) |
| `rig` | `vng_rig` | STATIC, glob | gfx, render | `Armature`, `Pose`, `SkinBinding`; the skinned-renderer CPO | [rigging.md](../rigging.md) |
| `text` | `vng_text` | STATIC, glob; TEXT | core (FreeType, HarfBuzz private) | `Font` shaping and rasterizing; the text-renderer CPO | [text_rendering.md](../text_rendering.md) |
| `ui` | `vng_ui` | STATIC, list; TEXT+UI | text, gfx, render (ICU private) | Retained widgets, `DrawList`, themes | [ui.md](../ui.md) |
| `editor` | `vng_editor` | STATIC, list; EDITOR | content, spatial | `EditableMesh`, topology, `MeshPatch`, the inspector wire format, selection, `limits.hpp` | [editor.md](editor.md) |
| `editor/preview.*` (2 files) | `vng_editor_preview` | STATIC; EDITOR, Linux | editor | Two-process preview transport (fds, shared memory) | [editor_boundaries.md](../editor_boundaries.md) |
| `opengl` | `vng_opengl` | STATIC, glob; OPENGL | gfx, render, glsl, resources (glad private) | `Device`, `Frame`, `Commands`, `GraphicsState`, `Program`, `GpuMesh`, `Image2D`, `Framebuffer`, `RenderTarget`, readback | [graphics_program.md](../graphics_program.md), [graphics_state.md](../graphics_state.md) |
| `render_opengl` | `vng_render_opengl` | STATIC, glob; OPENGL | render, analysis, glsl, opengl | `OpenGLProgramRuntime` (analysis capture), `SimpleMeshRenderer` | [analysis_rendering.md](../analysis_rendering.md) |
| `resources_opengl` | `vng_resources_opengl` | STATIC, list; OPENGL | opengl, providers | The transactional `MeshRenderer` (`update(device)`, then `commit()`) | [resources.md](../resources.md) |
| `bloom_opengl` | `vng_bloom_opengl` | STATIC, list; OPENGL | opengl, resources | The HDR bloom pyramid | [bloom.md](../bloom.md) |
| `rig_opengl` | `vng_rig_opengl` | STATIC, list; OPENGL | rig, render_opengl, resources | GPU skinning | [rigging.md](../rigging.md) |
| `text_opengl` | `vng_text_opengl` | STATIC, list; OPENGL+TEXT | opengl, text, resources | The glyph-atlas text renderer | [text_rendering.md](../text_rendering.md) |
| `ui_opengl` | `vng_ui_opengl` | STATIC, list; OPENGL+TEXT+UI | ui, text_opengl | The `DrawList` renderer | [ui.md](../ui.md) |
| `window` | **`vng_window_glfw`** | STATIC, glob; WINDOW_GLFW | core (GLFW private) | The GLFW window and input | [window_and_context.md](../window_and_context.md) |
| `glfw_opengl` | `vng_glfw_opengl` | STATIC, list; OPENGL+WINDOW_GLFW | opengl, window_glfw | `glfw_opengl::create_window(WindowDesc, ContextDesc, PresentationDesc)`, present, vsync | [window_and_context.md](../window_and_context.md) |

Where to start reading:
- [`examples/file_mesh_direct.cpp`](../../examples/file_mesh_direct.cpp) walks the whole
  path in about 180 lines: DSL stages → link → window → compile → GPU mesh → frame → state/run/view/draw → present.
- [codebase.md](../codebase.md) has "Follow a mesh from bytes to pixels" and "Choose the narrowest
  implementation boundary". [`docs/explore/codebase.html`](../explore/codebase.html) is the component
  catalog with exact direct links.
- To see the DSL, IR and GLSL for a small program without opening a window, run this. It prints the
  interfaces, the IR and the GLSL, then exits 1 when it cannot create the window (under 1 s):
  ```sh
  env -u DISPLAY -u WAYLAND_DISPLAY ./build/vng_triangle_demo --once
  ```

## 2. How the pieces connect

**Customization point objects (CPOs) plus ADL.** The complete dispatch mechanism is in `include/vng/render/program.hpp`:
```cpp
struct CompileProgram final {
    template<class Device, class Program>
    [[nodiscard]] auto operator()(const Device& device, const Program& program) const
        -> decltype(compile_graphics_program(device, program))
    { return compile_graphics_program(device, program); }
};
inline constexpr CompileProgram compile_program{};
```
- The unqualified call finds `opengl::compile_graphics_program` because `device` is an
  `opengl::Device`. So a backend hook must live in its argument's namespace. A missing or
  misplaced hook removes the CPO overload, and GCC reports
  "no match for call to '(const vng::render::CompileProgram) (...)'".
- The backend hooks are `make_backend_*`, `begin_backend_frame`, `upload_backend_image` and
  `compile_graphics_program`.
- To list the CPOs (the output also includes a few constants):
  `grep -rnE "inline constexpr [A-Z][A-Za-z]* [a-z_]+\{\};" include/vng`

**Namespaces do not follow directories.**

| Where | Namespace |
|---|---|
| Neutral modules | `vng::<dir>`. The core types are directly in `vng` (`vng::Vec3`, `vng::f32`). |
| `shader/dsl/` | `vng::dsl`, not `vng::shader::dsl` |
| `opengl/`, `render_opengl/`, `resources_opengl/`, `bloom_opengl/`, `rig_opengl/`, `text_opengl/`, `ui_opengl/` | `vng::opengl`, which ADL requires |
| `render_opengl/program_runtime.hpp`, `make_simple_mesh_renderer` | `vng::render` (`vng::render::OpenGLProgramRuntime`) |
| `glfw_opengl/` | `vng::glfw_opengl` |
| The renderer CPOs in `text/`, `ui/` and `rig/` | `vng::render` |
| The font and skin providers in `text/` and `rig/` | `vng::providers` |

**Layering rules.** `vng_layering_tests` (needs no build) enforces the first three. No test checks
the last two, so keep them by hand.
- `#include <vng/x/...>` only from a target that links `x`, directly or through PUBLIC or
  INTERFACE links. Files in `src/` may also use their target's PRIVATE links.
- The module include graph must be acyclic.
  - The test is titled "tree", but it only detects cycles.
  - The owner wants more than that: tree-like dependencies that are easy to spot, not a web
    of cross-links, and one clear owner per resource. Today's graphs are not all trees (see the
    "Project preferences to preserve" section of [documentation_work.md](../documentation_work.md)),
    so don't add cross-links.
- A relative include of another directory's private header is allowed only for the entries in
  `privateReachIns`, and the test compares that list *exactly*.
  - Never add an entry.
  - If you remove a reach-in, delete its entry too.
- Public headers never include `<glad/…>` or `<GLFW/…>`. Both are PRIVATE links.
  - A leaked include breaks consumers.
  - It also breaks any compile-fail test that includes the header (they compile with only
    `-I include`; some include `opengl/renderer.hpp` and `opengl/gpu_mesh.hpp`).
- `include/vng` uses angle-bracket includes only and never includes `examples/`. Application
  policy stays in `examples/` (see [codebase.md](../codebase.md)).

## 3. Errors, lifetimes and style

**Errors.**
- `Result<T> = std::expected<T, Diagnostic>` is declared per module: `shader`, `content`,
  `resources`, `timeline`, `ui`, `editor` (inspector) and `editor::preview`.
- `opengl` spells out `std::expected<T, opengl::Diagnostic>`. Its Diagnostic carries
  `driver_log`, `generated_source` and `source_map`.
- Other error types: `gfx::CameraDiagnostic`, `gfx::MeshDiagnostic`, `text::Diagnostic`,
  `rig::Diagnostic`, `window::Diagnostic`, and the `analysis::*Diagnostic` types (`Capture`,
  `Comparison`, `Sweep`, `Evidence`, `Export`).
- To cross a module boundary, use `resources::to_diagnostic(err)` or `resources::into_result(expected)`.
  The original error stays retrievable with `diagnostic.cause_as<E>()`, which returns a pointer
  or nullptr.
- Throw `std::invalid_argument`, `logic_error`, `length_error` or `out_of_range` only for broken
  preconditions. `vng_ui` also throws `runtime_error` when text measuring, shaping or ICU segmentation
  fails, and when the window input queue overflows.
- The engine never prints. The only exception is the `VNG_TRACE_SCROLL=1` trace in `src/window/glfw.cpp`.

**Lifetimes.** [window_and_context.md](../window_and_context.md) ("Lifetime and threading") and
[resources.md](../resources.md) cover these in depth.
- GPU types are move-only RAII objects.
  - They are created by `static create(...)`, `make_backend_*` or `upload_*` factories that return
    `std::expected`. The `make_backend_*_builder` hooks return a builder whose `build()` does.
  - Constructors are private. `Device` befriends the classes that need its `state_`.
- Frame-scoped handles hold `shared_ptr<ContextState>` plus a generation counter. A stale handle
  returns a diagnostic instead of crashing.
- Rvalue overloads are deleted (`Commands::run(Program&&)`, `graphics_state() &&`), so
  temporaries cannot be bound.
- Declare the window, then the `Device`, then the resources, so they are destroyed in reverse.
  An owner that pairs storage with a framebuffer relies on member order (see the comment in
  `opengl/render_target.hpp`).
- `resources::Shared<T>` is an immutable snapshot. Only its owner replaces it, e.g. via `MeshRenderer::update(device)` and the pending update's `commit()`.

**Style.** There is no `.clang-format`, `.clang-tidy` or `.editorconfig`, so never reformat whole files.
- Match the file you edit.
  - Older files use 4-space indentation with operators and statements spaced out: `core/types.hpp`, `shader/*`, `opengl/*`.
  - Newer files are dense, with several statements per line: `src/spatial/triangle_bvh.cpp`,
    `include/vng/editor/*`, `resources_opengl/*`.
- Naming:
  - types are PascalCase;
  - functions, variables and enumerators are snake_case;
  - private members end in `_`.
  - Exceptions: the `gfx::ComponentEncoding` and `AttributeInterpretation` enumerators (`Float16`,
    `FloatingPoint`), and UI/input polling (`isHovered()`, `changedText()`, `input::Frame::keyDown()`),
    which is camelCase.
- Headers use `#pragma once`, and almost everything is `[[nodiscard]]`.
- Float literals use an uppercase suffix (`1.0F`); the engine has no lowercase `f` literals.
- Keep the code `-Wconversion`-clean with explicit `static_cast`.
- Headers never contain a namespace-scope `using namespace`.
- Comments are sparse `//` lines that state a contract or explain why. There is no Doxygen.
- Tests use Catch2 v3, with full-sentence `TEST_CASE` names and tags such as `[shader][ir]`.

## 4. Shader path: DSL → IR → GLSL → GL

| Step | API | Notes |
|---|---|---|
| Interfaces | `shader::VertexInputs<Sem...>`, `VertexOutputs<ClipPosition, smooth<Sem>>`, `FragmentOutputs<shader::Color<0>>` | Semantics match by C++ type (`typeid`), not by name. `gfx::Color` (an attribute) ≠ `shader::Color<N>` (an output). |
| Stage | `shader::vertex<In,Out>("name", [](auto& s, dsl::Float4x4 model){...})` | The lambda runs **once**. Parameters after `s` become typed live arguments. The stage is validated, optimized (folding and dead-code elimination), then validated again. |
| Link | `shader::link(vertex, fragment[, shared_arguments])` | Matches varyings and assigns deterministic locations. Arguments are ordered vertex first, then fragment. |
| Emit | `glsl::emit(program)` → `ProgramSource` | Emits `#version 460 core` (the only `#version` in `src/` is in `src/glsl/emitter.cpp`) with names `vng_in_N`/`vng_out_N`. |
| Compile | `render::compile_program(device, linked)` → ADL → `opengl::compile_graphics_program` | Driver errors carry `generated_source` and `source_map`. |
| Draw | `commands.run(program, args...)`, `commands.view(view)`, `commands.draw(mesh)` | `run` needs the *exact* argument types, in order. `view` is required if the shader called `s.camera()`. |

- To inspect: `stage.dump_ir()`, `stage.dump_interface()`, `glsl::dump_source(*glsl::emit(program))`.
- The full explanations are in [shader_vertex_pipeline.md](../shader_vertex_pipeline.md) and
  [typed_shader_arguments.md](../typed_shader_arguments.md).

## 5. Verify an engine change

Run from the repo root in your own worktree's `build/`; see [build-and-test.md](build-and-test.md) for
the configure step and its etiquette. `ctest` never builds anything. The timings are for Debug on the
28-core dev machine.

```sh
# No build needed: layering + guide drift (<1 s). A failure that is already listed in status.md is not yours.
node --test tests/layering/layering.test.cjs docs/explore/tests/content.test.cjs
# No build needed: the 42 negative-API tests re-read the current headers (~3 s)
ctest --test-dir build -L compile-fail -j 8 --output-on-failure
# Which targets compile a header (from the last build's depfiles), i.e. what to rebuild and test
grep -rl --include='*.o.d' 'include/vng/opengl/device.hpp' build/CMakeFiles \
  | sed 's#build/CMakeFiles/##; s#\.dir/.*##' | sort | uniq -c | sort -rn
```

To syntax-check one translation unit with the exact build flags, without writing to `build/`, use the script in
[build-and-test.md §2](build-and-test.md#2-build) (under 1 s for `src/shader/ir.cpp`).

Rebuild, then run the engine tests:
- `core/types.hpp` reaches almost every translation unit, and `gfx/record.hpp` about 70% of them.
  Give the build a 10-minute tool timeout, or run it in the background, because the default 2-minute
  timeout is often too short.
- The ctest runs below use hidden windows or none (CPU ~0.3 s, GPU ~11 s). `vng_glfw_opengl_tests` runs directly,
  without its `[modes]` case, which briefly maps a fullscreen window.
```sh
cmake --build build -j 12 --target vng_timeline_tests vng_resources_tests vng_rig_tests \
  vng_text_tests vng_ui_tests vng_gfx_tests vng_render_tests vng_analysis_tests vng_content_tests \
  vng_document_tests vng_shader_tests vng_glsl_tests vng_opengl_tests vng_resource_owner_tests \
  vng_rig_opengl_tests vng_text_opengl_tests vng_ui_opengl_tests vng_render_opengl_tests vng_glfw_opengl_tests
ctest --test-dir build --output-on-failure \
  -R '^vng_(timeline|resources|rig|text|ui|gfx|render|analysis|content|document|shader|glsl)_tests$'
ctest --test-dir build --output-on-failure -j 4 \
  -R '^vng_(opengl|resource_owner|rig_opengl|text_opengl|ui_opengl|render_opengl)_tests$'
./build/vng_glfw_opengl_tests "~[modes]"
```

To iterate on one area, run a single binary with a Catch2 filter, for example
`./build/vng_opengl_tests "[graphics-state]"` or `./build/vng_shader_tests --list-tags`.

| You changed | Also run |
|---|---|
| `gfx` | `vng_gfx_tests`, compile-fail, `vng_opengl_tests` |
| `shader`, a new `glsl` lowering case (e.g. a new opcode) | `vng_shader_tests`, `vng_glsl_tests`, compile-fail, `vng_render_opengl_tests`, `vng_opengl_tests`. For a `glsl` change that alters existing GLSL output, use the next row. |
| `render`, `opengl`, `render_opengl`, `bloom_opengl`, existing `glsl` output | `ctest --test-dir build -L opengl -E e2e -j 4 --output-on-failure` (15 tests, also covering editor and scene rendering). `vng_editor_runtime_tests` alone takes ~90 s, so use a 5-minute tool timeout. The set includes `vng_editor_worker_tests`, which briefly shows the worker's window (Independent Play), and `vng_glfw_opengl_tests`, whose `[modes]` case briefly goes fullscreen: ask the owner first, or exclude both ([build-and-test.md §8](build-and-test.md#8-before-you-hand-off)). Then run the editor e2e tests if the editor draws differently. |
| `content` | `vng_content_tests`, `vng_document_tests`, `vng_spaceship_model_tests` |
| `timeline` | `vng_timeline_tests`, `./build/vng_editor_tests "[timeline]"`, `vng_tunnel_scene_tests` (builds, validates and round-trips the departure scene, the most heavily keyed one, ~12 s) |
| `spatial` | `./build/vng_editor_tests -# "[#picking_acceleration_tests]"` |
| `editor` (`src/editor`) | `vng_editor_tests`, `vng_editor_ui_tests`; `vng_editor_preview_tests` and `vng_editor_worker_tests` for `preview.*` (the worker tests briefly show a window) |
| `window`, `glfw_opengl` | `vng_window_glfw_tests` briefly **shows a window** on the owner's desktop, and `vng_x11_scroll_tests` starts Xephyr. Run them only for these directories. |

- "Skipped" is not "passed". GPU tests exit 77 without an OpenGL 4.6 context, and ctest still prints
  "100% tests passed". Read the list under "The following tests did not run".
- After any engine API change, grep the consumers in `examples/` and `tests/`. The editor and the scene
  libraries compile against the engine too.

## 6. Recipes

**Use a new vertex semantic.** This usually needs no engine change.
1. Declare the tag in your own code: `struct Tangent : vng::gfx::Semantic<vng::Vec4> {};`.
   Stock tags are in `gfx/geometry.hpp`.
2. Pick an encoding with `gfx::Record<..., gfx::as<Tangent, gfx::snorm8x4>>` (codecs are in
   `gfx/codecs.hpp`). Then add the tag to `VertexInputs<...>` and to any varyings.
3. Know the limits:
   - Attributes are only f32/i32/u32 scalars and 2–4 component vectors. Bool, matrix and record
     attributes are compile errors.
   - Integer varyings need `shader::flat<>`.
4. For `.vmesh` loading, map the tag with `content::vmesh::schema<Vertex>().map(...)`
   (see [mesh_and_vmesh.md](../mesh_and_vmesh.md)).

**Add a vertex codec or encoding.**
1. In `gfx/codecs.hpp`, add the codec, and a `default_codec` mapping if needed.
2. In `gfx/format.hpp`:
   - add the `ComponentEncoding` enumerator;
   - update `encoded_byte_size`, `encoded_byte_alignment`, `valid_vertex_format` and `vertex_format_compatible`.
3. Map the new encoding to `opengl::VertexComponent` in `src/opengl/gfx_vertex_input.cpp`. The
   GLenum mapping is in `src/opengl/vertex_array.cpp`.
4. Tests: `tests/gfx/gfx_tests.cpp`, `tests/opengl/gpu_mesh_tests.cpp`, and compile-fail
   `incompatible_codec`/`inconsistent_codec_format`.

**Add a DSL function or IR opcode.** `grep -rnw fract include/vng src` shows every place that needs editing
(`OpCode::fract` alone misses the enumerator and the DSL macro).
1. Add the enumerator to `OpCode` in `include/vng/shader/ir.hpp`. Its position is free: IR exists only in
   memory, nothing casts `OpCode` to an integer or writes it to disk, and dumps and diagnostics print
   `opcode_name`. Put it next to its relatives (e.g. after `mix`).
2. Add the DSL function in `include/vng/shader/dsl/functions.hpp` by copying the closest existing overload family,
   not by writing a generic template. Each family builds on `detail::intrinsic<T>(shader::OpCode::X, args...)`:
   - one float argument: `VNG_DSL_FLOAT_UNARY(name, opcode)`;
   - two same-type arguments: `VNG_DSL_BINARY_INTRINSIC` (as `min`, `pow`);
   - three same-type arguments: `VNG_DSL_TERNARY_SAME(name, opcode, requirement)` (as `clamp`, `mix`). Its overloads
     take the type and builder from the first `Expr` argument; the comment above the macro explains why they stay
     disjoint;
   - scalar operands next to a vector (GLSL's `clamp(vecN, float, float)`, `mix(vecN, vecN, float)`): an explicit
     `template<std::size_t N>` overload taking `detail::Operand<f32>` for the scalars, like the `clamp` and `mix`
     vector overloads below the macro. A constant *vector* operand cannot deduce `N`, which is why
     `VNG_DSL_CONSTANT_VECTOR_CLAMP` and `VNG_DSL_CONSTANT_VECTOR_MIX` spell out the widths;
   - host values enter only through the `consteval` constructors of `detail::Operand<T>` and `detail::Constant<T>`
     (`dsl/expr.hpp`), so a runtime `float` fails with "is not a constant expression". Pin the accepted and rejected
     shapes with a `requires` concept and `static_assert`s.
3. In `src/shader/ir.cpp`:
   - type-check it in `validate()`. Join the existing group of the same arity instead of adding a lone branch: unary
     (`negate || … || exponential`, whose final `else` accepts f32 scalars and vectors), binary
     (`dot || cross || minimum || maximum || power`) or ternary (`clamp || mix || select`). In the binary and
     ternary groups the last opcodes (`minimum`/`maximum`, `select`) are the fallback `else`, so add an explicit
     `else if (operation.opcode == OpCode::X)` arm before it. Reuse the local helpers (`require_operands`,
     `require_result`, `value_type`, `numeric`, ...);
   - add the name to `opcode_name`;
   - constant folding in `fold_operation` is optional and folds scalar constants only. It covers negate, not,
     `+ - * /`, comparisons, `min`/`max` and `select`, but not `clamp`, `mix` or the unary math.
4. Lower it in the `expression()` switch of `src/glsl/emitter.cpp`, e.g. `case OpCode::fract: return call_expression(operation, "fract");`.
5. Tests. Append a `TEST_CASE` to each of these; they are already in `CMakeLists.txt`, so no CMake edit is needed:
   - `tests/shader/sun_math_tests.cpp` (`vng_shader_tests`): `static_assert`s through a `requires` concept,
     `shader::validate`, and a broken copy (an operand popped or of the wrong type) that `validate` rejects;
   - `tests/glsl/sun_math_tests.cpp` (`vng_glsl_tests`): the GLSL name appears in the emitted text;
   - `tests/opengl/sun_math_tests.cpp`, compiled into `vng_render_opengl_tests` (not `vng_opengl_tests`): the driver
     compiles it and the pixels have the expected values.

   Also extend the lists that enumerate intrinsics: the opcode loop in "GLSL scalar and vector intrinsics reject
   matrix operands" (`tests/shader/shader_tests.cpp`; its helper `make_matrix_intrinsic_module` gives every opcode
   except `minimum`/`maximum` three operands, so extend it for another arity), and, for functions that accept
   constant operands, the `producer(ir, …)` checks in `tests/shader/constant_operand_tests.cpp`. Add a compile-fail
   `literal_*` case for constant-only operands ([build-and-test.md §7](build-and-test.md#7-add-tests-and-targets)).
6. Remove the function from the "There is no …" list in gotcha 13 (§7). `docs/typed_shader_arguments.md`
   ("Constant-only implicit operands") names a few mixed helpers as examples; add yours if it is one.

**Add a graphics-state setting.**
1. Add the neutral type in `include/vng/render/graphics_state.hpp`. A backend-only setting goes in
   the backend namespace instead, like `opengl::PolygonMode`.
2. In `include/vng/opengl/graphics_state.hpp`:
   - add a `set(NewSetting)` overload to `detail::GraphicsStateAccess`;
   - add the field to `GraphicsStateSnapshot`.
3. In `src/opengl/context_state.hpp`, add a cached `graphics_<x>` field.
4. In `src/opengl/graphics_state.cpp`:
   - reset the default in `validate()`;
   - apply it in `synchronize()`;
   - implement `set()` (copy `set(PolygonMode)`);
   - include it in `snapshot()` and in `set(GraphicsStateSnapshot)`.
5. Consider `src/opengl/render_state_scope.cpp` (native save and restore) and
   `OpenGLProgramRuntime::effective_raster` (analysis capture).
6. Tests:
   - `tests/render/graphics_state_tests.cpp` (the concept `SupportsGraphicsSetting`);
   - `tests/opengl/graphics_state_tests.cpp` (`[graphics-state]`, pixels).
7. Update [graphics_state.md](../graphics_state.md).

**Add a GPU resource type.** Use `RenderTarget` and `Image2D` as models.
1. Add the neutral description and a CPO in `gfx/` or `render/`. Copy `MakeTarget` in
   `render/target.hpp`: it forwards to an unqualified `make_backend_target(...)`.
2. Add `include/vng/opengl/x.hpp` and `src/opengl/x.cpp`; the glob picks up the `.cpp`. The class must be:
   - move-only;
   - created by `static create(const Device&, …) -> std::expected<X, Diagnostic>`;
   - able to answer `belongs_to(const Device&)`.
3. Its destructor must leak the handle and record a lifecycle diagnostic when the context is not
   current. See `Image2D::release_noexcept`.
4. Add a free `make_backend_x` in `vng::opengl`, and a `Device` friend entry if the class needs `state_`.
5. Optionally, add a provider in `include/vng/providers/` (e.g. `RenderTargetProvider`).
6. Tests: `tests/opengl/image_resource_tests.cpp`, `tests/opengl/resource_owner_tests.cpp`,
   `tests/resources/provider_tests.cpp`.

**Add an engine module directory.**
1. In `CMakeLists.txt`, add `add_library(vng_<dir> …)`, `add_library(vng::<dir> ALIAS vng_<dir>)`,
   `target_link_libraries(...)` and, for a compiled (STATIC) target, `vng_target_defaults(vng_<dir>)`,
   inside the right option `if()`.
2. Add `<dir>: "vng_<dir>"` to `moduleTargets`. Otherwise the layering test fails with
   "add its directory to moduleTargets".
3. Re-run `cmake -S . -B build`. A new target is unknown until reconfigured, and the build fails with "No rule to make target".
4. Add a test executable (see [build-and-test.md](build-and-test.md)).
5. Add a `component(...)` entry in `docs/explore/codebase.js` and at least two sections in
   `docs/explore/codebase-details.js`. Mention the module in [codebase.md](../codebase.md). See
   [docs-maintenance.md](docs-maintenance.md).

**Add a `.cpp` file or a test file.**
- *list* targets (see §1) ignore new files until you add them to `CMakeLists.txt`.
- Test executables list their sources explicitly; only `tests/rig/*` and `tests/text/*` are globbed.
- A GPU test must create its context with `vng::test::create_hidden_opengl_window`
  (`tests/support/glfw_opengl.hpp`) and call `std::exit(77)` when that fails.

**Change a `target_link_libraries` line.**
- `vng_guide_tests` requires the link set of every mapped target to equal its dependency list in
  `docs/explore/codebase.js` exactly, including PUBLIC/PRIVATE/INTERFACE.
- Update the entry, then run the node checks.

## 7. Gotchas

1. **A new opcode with no `validate()` branch passes validation silently.** The chain has no `else`.
   `opcode_name` and the emitter `switch` have no `default`, so you only get a `-Wswitch` *warning*.
2. **Warnings never fail the build.** `vng_target_defaults` adds `-Wall -Wextra -Wpedantic -Wconversion`
   but not `-Werror`, and INTERFACE targets get no flags at all. Read the build output.
3. **Host values in DSL expressions must be compile-time constants of the exact type.**
   - `x * brightness` (a runtime `float`) fails to compile, and so does `x * 2.0` (a `double`).
   - Use a typed lambda parameter (`dsl::Float brightness`) for a live value.
   - Use `s.constant(value)` for a baked snapshot.
4. **`commands.run(program, args...)` checks the exact types.** `run(p, model, 2.0)` does not compile;
   pass `2.0F`.
5. **Your semantic name never appears in the GLSL.** The generated names are `vng_in_N`/`vng_out_N`. Use `dump_interface()` instead.
   Two `struct Color` tags in different namespaces are different semantics.
6. **There are two sets of depth and cull types.**
   - `render::DepthState` (test/write default **off**), `render::DepthCompare`, `render::CullMode` and
     `render::FrontFace` belong to the per-frame `GraphicsState`.
   - `opengl::DepthState` (default **on**), `opengl::DepthCompare`, `opengl::CullState`, `opengl::CullMode`
     and `opengl::FrontFaceWinding` belong to the immediate `Device::set_depth_state`/`set_cull_state`.
   - Inside `namespace vng::opengl`, an unqualified `DepthState`, `DepthCompare` or `CullMode` means the opengl one.
   - After raw or immediate state changes, call `commands.restore()` and upload the view again.
7. **Every frame starts from defaults:** depth off, no culling, blending off, counter-clockwise
   front faces, fill. Set depth explicitly for every 3D pass.
8. **Only one active `Frame` per context.** A second `begin_frame` fails with "begin_frame rejected an overlapping active frame".
9. **No resource replacement while a frame is active.** `reload`, `resize`, `create` and a `MeshRenderer` update's `commit()` fail
   with "finish the active frame before replacing resources".
10. **Destroying a GPU object needs its context to be current.**
    - Otherwise the handle is deliberately leaked and a lifecycle diagnostic is recorded. Read it with
      `Device::take_lifecycle_diagnostics()`.
    - Destroying a GLFW window on a non-owner thread terminates the process.
11. **`gfx::Camera` setters never validate.** Errors surface only in `camera.snapshot(extent)` or
    `RenderView::create`. For a camera-reading shader, `commands.view()` rejects a `RenderView::without_camera`,
    and `draw` then fails because no camera view was uploaded.
12. **Reversed depth is chosen per frame** (`render::FrameDesc::depth_mapping`).
    - Generated camera projection adapts automatically.
    - Hand-written depth math must use `render::encode_depth` and `encode_compare`.
    - [graphics_state.md](../graphics_state.md) does not cover this; [renderer_api.md](../renderer_api.md) mentions it.
13. **What the shader DSL lacks.**
    - Stages are vertex and fragment only. There is no compute and no DSL control flow: `if_region`,
      `loop_region` and `yield` exist in the IR, but GLSL lowering rejects them.
    - Texture access is `sample_2d<Binding>` (fragment only) and `sample_2d_lod<Binding>`. Storage is
      `matrix_buffer<Binding>`, a read-only `Mat4` array.
    - There is no `length`, `smoothstep`, `step`, `tan`, `atan` or `log`. Compose them from
      `sqrt`, `dot`, `clamp`, `mix`, `select`, `min`/`max`, `pow`, `exp`, `sin`/`cos`, `floor`/`fract`.
14. **`core` has no vector arithmetic.** `Vec3 + Vec3` does not compile. Consumers write small local
    helpers, and there is no shared vector math library (`rig/math.hpp` has only rigging helpers:
    `Mat4` `multiply`, `transform_point`/`transform_vector`, `inverse_affine`, `normal_matrix`).
15. **Timeline values are a variant.** `Value = variant<bool,i32,u32,f32,Vec3,string>`.
    - Only f32 and Vec3 interpolate: linear or shape-preserving cubic (Smooth in the editor).
    - The *arriving* key owns the interpolation of the segment before it, and the default is `hold`.
    - For the key limits, see [limits-and-non-features.md](limits-and-non-features.md).
16. **The editor's 64 MiB scene limit is an engine constant.** It is `vng::editor::max_document_bytes`
    in `include/vng/editor/limits.hpp`, defined as the preview IPC's `max_message_bytes` (64 MiB) minus
    256 bytes for protocol headers. Raising it touches the transport.
17. **The guide quotes engine headers verbatim**, ignoring whitespace (among them `shader/ir.hpp`, `shader/stage.hpp`,
    `opengl/backend.hpp` and `editor/preview.hpp`). Editing a quoted snippet fails `vng_guide_tests`; update the
    excerpt in the same change. The full list, the command that re-lists it and the fix recipe are in
    [docs-maintenance.md §3](docs-maintenance.md#3-quoted-code-drift-checked-excerpts).
18. **`if(EXISTS <first test file>)` guards silently drop whole test executables.** They guard
    analysis, content, shader, glsl, render_opengl, window and *all* GPU engine tests
    (`tests/opengl/opengl_tests.cpp`). A similar guard on `examples/triangle.cpp` drops every graphical
    example, the editor app and its runtime and e2e tests. `vng_glsl` also falls back to an INTERFACE library if
    `src/glsl/` has no `.cpp`. After renaming a test file, check that `ctest --test-dir build -N` still shows the same count.
19. **Keep `ContextDesc::samples = 0`.** No test creates a multisampled default framebuffer (only a
    multisample `Renderbuffer` is tested), and framebuffer readback rejects multisample attachments
    ("resolve it first").
