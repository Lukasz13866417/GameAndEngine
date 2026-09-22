/* Concrete API and lifetime notes augment the build/ownership map.
 * Keep snippets source-checked where exact=true. All other snippets are sketches.
 * This is curated documentation, not generated API or inferred include analysis. */
(() => {
  "use strict";
  const flatten = nodes => nodes.flatMap(node => [node, ...flatten(node.children || [])]);
  const nodes = flatten(window.VNG_GUIDE);
  const byTarget = new Map(nodes.filter(n => n.component).map(n => [n.component.target, n]));
  const section = (title, paragraphs, source, code, exact = false) => ({
    title, paragraphs: Array.isArray(paragraphs) ? paragraphs : [paragraphs], source, code, exact
  });
  const describe = (target, sections) => { byTarget.get(target).sections = sections; };

  describe("vng_core", [
    section("Values, not engine services", "i32, u32 and f32 name the widths used by records and shader contracts. Vec2/3/4 and Mat3/4 are small value types; matrices store columns. A CPU Vec3 is ordinary data, whereas dsl::Float3 records a shader calculation. Sharing the logical type does not make their execution or storage interchangeable.", "include/vng/core/types.hpp"),
    section("Units and clocks stay explicit", "Use degrees(...) or radians(...) for angle intent rather than guessing an unlabelled float's unit. The monotonic clock is for elapsed intervals, not dates or serialized timeline identity. This target has no application, scene, renderer or global resource owner to initialize.", "include/vng/core/monotonic_clock.hpp")
  ]);
  describe("vng_gfx", [
    section("Semantic → record → stream", [
      "A semantic tag names what a value means. Its C++ type is its identity: two tags carrying Vec3 can mean Position and Normal without being confused. Record<...> calculates offsets/stride and owns byte storage; as<Tag, Codec> changes one field's encoding, not its logical meaning.",
      "VertexStream<Record> owns a contiguous vector of records. stream[i].set(Tag{}, value) encodes a field; get decodes it. bytes() exposes the upload span, not another allocation. Growing the stream can invalidate references and byte spans. There is no GPU buffer until an explicit backend upload."
    ], "include/vng/gfx/vertex_stream.hpp", `using Vertex = vng::gfx::Record<
    Position,
    vng::gfx::as<Color, vng::gfx::unorm8x4>>;`, false),
    section("A mesh owns geometry, not scene identity", "Mesh<RecordTypes...> combines typed streams, indexed faces and optional authored edges. vertices<Record>() chooses a stream when there are several; validate() checks equal vertex counts and index validity. A mesh is not a scene instance: per-instance transforms, visibility and blueprint identity belong above it. Vertex encodings are not a future uniform/storage-buffer ABI.", "include/vng/gfx/mesh.hpp"),
    section("A camera describes a view without knowing OpenGL", "Camera owns pose and projection settings. Position/look_at describe where it is and looks; perspective field of view describes optical zoom. Near/far planes control clipping, not movement speed. RenderView::create(camera, extent) validates a concrete viewport-sized view that commands can upload. Editor walk/orbit behavior is application policy layered on this value.", "include/vng/gfx/camera.hpp", `vng::gfx::Camera camera;
camera.set_position({2.6F, 2.0F, 3.2F})
    .look_at({0.0F, 0.0F, 0.0F})
    .set_perspective({
        .vertical_fov = vng::degrees(50.0F),
        .near_plane = 0.1F,
        .far_plane = 100.0F,
    });`),
    section("Resolving is an implementation boundary", "Shader inputs demand semantic types; a vertex layout supplies locations, offsets, strides, formats and divisors. Resolution matches those contracts independently of physical field order. GPU mesh helpers prepare/cache the matching vertex input, so normal callers do not manually build VAO attribute tables or expose resolved stream lists.", "include/vng/gfx/vertex_layout.hpp")
  ]);
  describe("vng_spatial", [
    section("Build once, query many times", "TriangleBvh owns local-space vertices, triangle references and its acceleration nodes. Construction partitions triangles; intersect(ray) skips entire bounds that the ray misses. This accelerates triangle work inside a mesh. It does not automatically replace the editor's candidate-instance traversal with a scene-wide BVH.", "include/vng/spatial/triangle_bvh.hpp"),
    section("Why the ray is not always normalized", "Transforming a world ray into an instance's local coordinates changes the direction's length. Keeping that transformed direction preserves the ray parameter, so hit distances can still be compared correctly under scaling. EditableMesh shares its immutable picking index until geometry changes; invalidation causes a rebuild on the next query. Invalid BVH input is rejected, including via invalid_argument: not every low-level API here returns expected.", "tests/editor/picking_acceleration_tests.cpp")
  ]);
  describe("vng_timeline", [
    section("A track is a property over time", "Target identifies an object and property string. Track adds a label, layer and sorted Keyframe values. Timeline::set inserts/replaces a timestamp; sample asks what that property is at a timestamp. Supported Value alternatives include booleans, fixed-width integers, f32, Vec3 and strings. The timeline contains data, not object pointers or effect callbacks.", "include/vng/timeline/timeline.hpp"),
    section("Incoming interpolation belongs to the arriving key", "Each key chooses hold or linear from the preceding key. Linear is accepted only for f32 and Vec3; discrete tracks must use hold. Before a track's first key there is no override; at a key the key's value wins; after the last key its value holds. Editor rules such as protected time zero and edit-only-at-keyframes are enforced by the authoring layer, not by this general sampler.", "src/timeline/timeline.cpp"),
    section("Sampling is not a simulation update", "Layers organize tracks; they do not themselves decide visibility or enable/disable sampling. The caller chooses the playback clock and applies sampled values. Timeline does not run game logic, clamp elapsed time, draw a frame or synchronize a worker. Mutation can invalidate returned track/key views, so do not retain those views across edits.", "tests/timeline/timeline_tests.cpp")
  ]);
  describe("vng_resources", [
    section("Recipe and ownership answer different questions", "A provider's provide(...) creates a resource. The owner decides when to ask again, when to install the replacement and when to release the old resource. Reconstruction information belongs in the provider; raw Buffer/Image2D need not keep uploaded bytes. Moving a ready GPU object transfers ownership but does not magically discover its recipe.", "include/vng/resources/provider.hpp"),
    section("Three useful forms", "Provider<T, Context, ...> retains a shared type-erased recipe with the declared call signature. Shared<T> holds a read-only shared snapshot; replacing one owner's handle does not mutate every existing borrower. Provided<T, P> packages a ready value together with its provider via provided(value, source). Packaging does not invoke the provider or upload the value again.", "include/vng/resources/shared.hpp"),
    section("No hidden recursive global manager", "The provide helper adapts provider calls; it may omit a context argument only when the provider signature permits it, never discard a required request. Reload orchestration and transactional failure behavior belong to each concrete owner. A provider may be reused across owners, but that does not turn those owners into an implicit graph of mutable global resources.", "tests/resources/provider_tests.cpp")
  ]);
  describe("vng_render", [
    section("A renderer is a drawing policy with an explicit backend", [
      "Derive from Renderer<Ticket, Backend> and provide render(frame, view, span<const Ticket>). The base stores no resources, contains no virtual functions and cannot be used as a plain renderer. Tickets contain per-submission information; your subclass owns persistent programs/buffers and chooses batching, state changes and subordinate renderers.",
      "Backend is a compile-time identity exposing device_type and frame_type. opengl::Renderer<Ticket> is the short alias for the OpenGL specialization. There is no default 'portable' backend: portable algorithms are templates over B; an instantiated renderer's actual resources belong to one backend."
    ], "include/vng/opengl/renderer.hpp", `template<class Ticket>
using Renderer = render::Renderer<Ticket, Backend>;`, true),
    section("Compatibility is checked before rendering", "RendererType checks public inheritance and ticket identity. RendererFor<R, F> also requires a real frame, matching backend identity and a callable batch render. render_one makes a one-ticket span without requiring every subclass to repeat an overload. SameBackend checks API compatibility only: two OpenGL devices can still have incompatible contexts, which runtime validation must reject.", "include/vng/render/backend.hpp"),
    section("Neutral facade → concrete overload", "compile_program(device, program), begin_frame(...) and feature builders forward to implementation hooks found through the concrete device type. This is static C++ dispatch, not a runtime service locator. A future backend supplies its identity, types and supported hooks. Merely changing a type alias does not port OpenGL calls inside a renderer.", "include/vng/render/program.hpp"),
    section("Live graphics state is independent of the shader", "commands.graphics_state().set(DepthTest{false}) changes that setting for following draws while preserving unrelated choices. GraphicsState<BackendState> forwards only supported setting types; backend-only settings remain in their backend namespace. run(program, args...) selects shaders and arguments, not a persistent graphics preset. A renderer may set depth/culling/blending differently for each batch.", "include/vng/render/graphics_state.hpp", `auto graphics = commands.graphics_state();
auto changed = graphics.set(vng::render::DepthTest{false});
if (!changed) return changed;
// Following draws use the updated depth-test setting.`)
  ]);
  describe("vng_analysis", [
    section("Evidence has an identity", "Analysis types describe frame identity, item/primitive correspondence, observations and captured image data. An item ID identifies a submitted logical item; a primitive ID identifies a surface primitive within it. These are useful only with the matching capture metadata, not as timeless pointers into an editable document.", "include/vng/analysis/manifest.hpp"),
    section("Observation is separate from ordinary output", "A shader may name values with stage.observe(Tag{}, expression). Production rendering still returns ordinary fragment outputs; enhanced emission can additionally record those observations and surface identity. The neutral evidence model does not allocate framebuffers or invoke OpenGL. Program runtime and concrete renderer code connect the metadata to actual capture resources.", "include/vng/analysis/analysis.hpp")
  ]);
  describe("vng_content", [
    section("Mesh extraction is an explicit string → semantic mapping", "The file API owns parsing and typed extraction. A schema states which named field feeds each record semantic; it does not assume that position is always field zero or Color always has the same encoded bytes. load(path, schema) returns a CPU mesh or a diagnostic before any GPU context is needed.", "examples/file_mesh_direct.cpp", `namespace vmesh = vng::content::vmesh;
auto vertex_schema = vmesh::schema<Vertex>();
vertex_schema.map("position", Position{});
vertex_schema.map("color/0", Color{});`, true),
    section("General documents intentionally know little about scenes", "read_document(path) parses the document; root().get<float>(\"exposure\") requests a typed value. child(), elements() and members() navigate structure without requiring a predeclared engine object type. The caller checks document kind and decides what an object means. A checked Reader callback can assemble a plain settings struct while the outer read operation returns expected with source-location diagnostics.", "examples/document.cpp"),
    section("File preservation is not live resource management", "The generic representation can preserve unknown fields when the application edits known ones. Writing emits canonical document text; it is not a promise to preserve original comments/whitespace byte-for-byte. Decoded images and mesh documents are CPU resources. GPU creation, provider retention and scene-file transaction policy belong to other components.", "include/vng/content/document.hpp")
  ]);
  describe("vng_shader", [
    section("The lambda builds a program; it is not the draw callback", [
      "vertex<Input, Output>(...) and fragment<Input, Output>(...) execute their lambda once. The stage records typed operations, validates its output and returns expected. link(...) checks matching semantic types/interpolation and assigns interfaces. Retain the resulting program, not Expr handles from the temporary builder.",
      "Expr<T> contains a FunctionBuilder pointer and ValueId. Copying it copies a handle; operators append operations using operand builders. There is no public Var, implicit host bool conversion or per-expression virtual dispatch. Mixed-builder expressions produce build diagnostics rather than accidentally combining unrelated shaders."
    ], "include/vng/shader/dsl/expr.hpp"),
    section("Live arguments are ordinary CPU values at submission", "A lambda parameter such as dsl::Float brightness becomes a typed program argument. commands.run(program, brightness) supplies the current CPU f32 each draw. Constants/literals become recorded constants; captured mutable C++ variables are not a live binding. Consteval literal operands reject accidental ordinary runtime operands, while explicit stage constants make snapshot intent visible.", "examples/file_mesh_renderer.cpp", `auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
    "file_mesh_fragment",
    [](auto& stage, vng::dsl::Float brightness) {
        const auto source = stage.input(Color{});
        const auto color = vng::dsl::vec4(source.xyz() * brightness, source.w());
        stage.observe(SurfaceColor{}, color);
        return stage.output(
            field<vng::shader::Color<0>>(color));
    });`, true),
    section("IR is the recorded calculation, not generated GLSL text", "ModuleIR tables hold types, values, operations and regions addressed by typed IDs. An operation names its opcode, operands, result, optional nested regions, payload, effect classification and source origin. Multiple users can share one producer. Ordered regions allow effects/control structure without replacing the representation. dump_ir exposes this form before any backend emission.", "include/vng/shader/ir.hpp", `struct ValueNode {
    TypeId type;
    OperationId producer;
};`, true),
    section("Packing follows logical values, not vertex bytes", "Arguments<Ts...> records the CPU call signature. ArgumentPack flattens arguments into logical words: vectors by component, matrices by column, records by decoded semantic values. Its views borrow the pack and cannot safely outlive it. OpenGL chooses uniform lowering later. A normalized 8-bit vertex Color and a Vec4 shader argument have the same logical value but different storage paths.", "include/vng/shader/arguments.hpp")
  ]);
  describe("vng_providers", [
    section("Retain the information needed to recreate", "providers::program(shaderProgram) retains the neutral program and compiles a backend program when provided with a device. Mesh/image/font/target providers retain or describe their own source data. Choose a recipe matching the source's lifetime; a temporary borrowed byte span is not a durable reload recipe.", "include/vng/providers/program.hpp"),
    section("Recipes plug into owners and builders", "A builder receives providers while assembling a ready owner; later that owner can call them again for reload. The provider has no hidden authority over already-built owners. For example a mesh provider is passed to mesh_renderer_builder, which creates and retains the GPU realization and recipe together.", "examples/glow.cpp", `auto ring_builder = render::mesh_renderer_builder(device);
ring_builder.mesh(providers::mesh(scene::ring()));
auto rings = ring_builder.build();
if (!rings) return fail(rings.error());`, true)
  ]);
  describe("vng_rig", [
    section("Rest structure, pose and weights are different data", "Armature defines bone names, parents and rest transforms. Pose contains changing local transforms for one animation state. SkinBinding associates a mesh snapshot with one armature and its vertex influences. Bone position does not live in the weights; weights say how strongly a bone affects a vertex.", "include/vng/rig/skin_binding.hpp"),
    section("Build a hierarchy before binding a mesh", "ArmatureBuilder checks the completed hierarchy at build(). A bind operation constructs the mesh/armature association; per-vertex influences are validated there. Separate poses can reuse the same rest structure and mesh binding. No GL context is involved until the rendering integration realizes the binding.", "examples/rigging.cpp", `rig::ArmatureBuilder bones;
const auto root = bones.add_bone("root");
const auto middle = bones.add_bone("middle", root, {.translation = {0, 1, 0}});
const auto tip = bones.add_bone("tip", middle, {.translation = {0, 1, 0}});
auto armature = bones.build();`, true)
  ]);
  describe("vng_text", [
    section("Text becomes glyphs before it becomes triangles", "Font holds shared font data. layout(utf8, pixelSize) shapes text into positioned glyphs; measure asks for metrics; rasterize(glyphIndex, pixelSize) produces a glyph bitmap. HarfBuzz chooses glyphs/positions and FreeType supplies rasterization. A glyph index is not a Unicode code point, so bypassing shaping breaks many scripts and ligatures.", "include/vng/text/font.hpp"),
    section("Know the current scope", "The font/shaping layer handles runs, not a complete publishing layout system. Do not assume automatic paragraph-level bidirectional layout, arbitrary font fallback, wrapping or color-emoji composition. The OpenGL renderer owns atlas caching; a Font itself does not own a GL texture or need a current context.", "src/text/font.cpp")
  ]);
  describe("vng_ui", [
    section("Screen owns widgets; handles refer to them", "Construct a Screen with a theme, create containers/widgets and keep lightweight handles in a panel. Handles carry identity and refer back to Screen state; they are not owners of independent widget trees. Layout/input produces a neutral DrawList, which a renderer consumes separately.", "include/vng/ui/ui.hpp"),
    section("Update first, then poll behavior", "ui::update feeds current native input and elapsed time into Screen. Poll clicked() for an activation, isPressed() for current press state, changedText() for edits and submittedText() for a submission. Text results can be borrowed string views: copy them when storing beyond the immediate use. Ordinary if statements can drive application operations without routing callbacks through a global bus.", "examples/ui.cpp", `if (auto text = name.changedText()) {
    status.text("Name changed: " + std::string(*text));
}`),
    section("UI state is not authored scene state", "Focus, hover, scrolling and open menus belong to the Screen/panel layer. A scene property changes only when a control issues an EditingSession operation. ICU supports text-editing boundaries; it does not make the UI renderer responsible for document validation, undo or IPC.", "tests/ui/ui_tests.cpp")
  ]);
  describe("vng_editor", [
    section("Editable geometry is reusable outside this editor app", "EditableMesh keeps the vmesh document and selected position field, retaining other attributes during edits. Position operations can invalidate a shared picking index. Topology operations validate a candidate before replacing geometry: fill makes an edge for two vertices or triangulates a polygon for more; subdivide inserts shared edge midpoints; align_to_line leaves the first two defining points fixed.", "include/vng/editor/mesh.hpp"),
    section("Selection and inspector description are data", "Selection helpers manage element identities independently of UI widgets. Schema describes groups, fields, actions and gizmos. Inspector events carry values and phases such as begin/update/commit/cancel, not a pointer to a lambda living in another process. Object/generation/revision/context stamps allow the receiver to reject stale interactions.", "include/vng/editor/inspector.hpp"),
    section("Application policy stays above the library", "This library does not decide whether mesh edits are drafts, whether a timestamp is editable or what deleting an instance does to its blueprint. EditingSession and Document implement those rules. Add a general mesh operation here; add an editor-specific command/undo effect in the application project layer.", "src/editor/mesh_topology.cpp")
  ]);
  describe("vng_editor_preview", [
    section("The UI owns the process connection", "PreviewSession starts/stops worker and build processes, sends control messages and receives complete RGBA frames. poll() lets the UI observe progress without waiting for compilation or rendering. WorkerEndpoint exposes the other side. The shared image transport is CPU-visible pixel data, not a shared mutable C++ scene or a cross-process OpenGL object.", "include/vng/editor/preview.hpp"),
    section("Reliable edits and replaceable viewing requests differ", "Authored commands must not disappear. send_latest is for an absolute newest viewport request that can supersede an older one; sending incremental deltas through it would lose motion/edits. Application PreviewUpdates handles revision acknowledgment and narrow patch coalescing above this transport. Framing/build-generation failures are returned as diagnostics rather than silently treating stale results as current.", "src/editor/preview.cpp"),
    section("Isolation is for iteration, not untrusted code", "A bad effect can terminate the rendering worker while the UI survives and rebuilds it. This is not a security sandbox: project C++ runs with the user's process permissions. Protocol payloads are serialized values and explicitly versioned messages; pointers, widget handles and callbacks never cross the process boundary.", "tests/editor/preview_tests.cpp")
  ]);

  describe("vng_glsl", [
    section("Source generation works without a window", "The emitter consumes linked shader IR and produces vertex/fragment StageSource values. Each contains readable GLSL, input/output metadata, used texture/matrix-buffer slots and a source map. Deterministic operation names and explicit locations make a dump useful when comparing runs. Creating a Device is not necessary to inspect emitted shader text.", "include/vng/glsl/emitter.hpp"),
    section("Logical arguments are lowered here", "ParameterMetadata tells the backend how CPU argument words correspond to uniforms. A record is flattened into logical leaves and rebuilt in GLSL; its packed vertex representation is never treated as a uniform ABI. Driver compilation happens in vng_opengl and uses source maps to attach generated-line failures to recorded operations.", "src/glsl/emitter.cpp"),
    section("Enhanced emission does not rerun the lambda", "AnalysisEmission requests additional item/primitive/observation outputs from the same linked program. The backend adds flat item identity plus fragment attachments and metadata. A first-item uniform combined with gl_InstanceID identifies instances in one draw. The original program stays unchanged; matching target attachments and readback are the runtime's responsibility.", "include/vng/glsl/emitter.hpp", `struct AnalysisEmission final {};`, true)
  ]);
  describe("vng_opengl", [
    section("Device validates where GL operations may run", "Device::create receives current-context access, loads procedures and validates the required OpenGL context/thread. It does not create a GLFW window. GPU objects must be created, used and destroyed while a compatible owning context is current. Same backend type is not enough: two unrelated OpenGL contexts are still different resource domains.", "include/vng/opengl/device.hpp"),
    section("Frame scopes access; Commands borrows it", "begin_frame selects a target and frame description. frame.render_context() returns Commands borrowing the active frame state. Copies share the same binding/state bookkeeping; they are not separate command buffers or independent state snapshots. Ending the frame invalidates command access. Do not retain Commands into another frame, and resize/reload attachments between frames rather than while borrowed.", "include/vng/opengl/commands.hpp"),
    section("The draw sequence has separate jobs", "graphics_state().set changes live state. run(program, args...) selects a typed program and uploads the current arguments; bind is the more explicit granular binding operation. view uploads camera data, draw submits geometry, and present swaps a window afterwards. The cache avoids redundant work, but raw GL calls can disturb it: use the documented restore path when interleaving lower-level code.", "examples/file_mesh_direct.cpp", `if (auto bound = commands.run(*program, model, brightness); !bound) {
    return example::fail(bound.error());
}
if (auto viewed = commands.view(*view); !viewed) {
    return example::fail(viewed.error());
}
if (auto drawn = commands.draw(*gpu_mesh); !drawn) {
    return example::fail(drawn.error());
}`, true),
    section("Mesh geometry and instance data have different update rates", "GPU mesh resources retain buffers and prepared vertex-input state. InstanceBuffer<Record> is a separate reusable allocation; update grows capacity geometrically and uploads records without recreating shared geometry. Instanced draw resolves those semantic inputs with divisor one. Per-instance transforms/colors/IDs can therefore vary within one geometry batch.", "include/vng/opengl/instance_buffer.hpp"),
    section("A render target owns a coherent attachment set", "RenderTarget keeps its provider, color/depth images and framebuffer together. Its creation/resize/reload validates a complete replacement instead of leaving mismatched extents. The framebuffer is destroyed before its attachments; all still require the correct context. A captured image or readback is CPU evidence, not a promise that those GPU attachments stay alive.", "include/vng/opengl/render_target.hpp")
  ]);
  describe("vng_window_glfw", [
    section("Window and input are their own boundary", "GlfwWindow owns a native window, polls events and exposes input/framebuffer size. Generic Window capabilities do not require every window to present graphics: an input-only window remains possible. OpenGL context tokens and swap policy are deliberately supplied by the separate GLFW/OpenGL integration.", "include/vng/window/window.hpp"),
    section("Input uses current window state", "Call window.poll_events() through the instance-facing API, then update input/UI and read the framebuffer extent. Framebuffer size, not merely logical window size, controls pixel rendering on scaled displays. Application loops decide when to render or sleep; native windowing does not own EditingSession, timeline playback or GPU caches.", "include/vng/window/glfw.hpp")
  ]);
  describe("vng_glfw_opengl", [
    section("The bridge is allowed to know both sides", "glfw_opengl::Window composes the native window with context lifetime. make_current returns CurrentContextAccess for Device creation; release_current and present remain explicit operations. Window/context must outlive all GPU objects. The example GlfwOpenGLSession declares window before device so reverse destruction keeps that order safe.", "include/vng/glfw_opengl/glfw_opengl.hpp"),
    section("Vsync is presentation intent", "window::VSync::on/off is neutral vocabulary; Presentable requires set_vsync, vsync and present. The integration translates the request for the current owning context. It does not silently switch contexts; drivers/compositors may override behavior. vsync() reports the requested mode, not measured refresh pacing, and is separate from an application FPS cap.", "include/vng/window/presentation.hpp", `enum class VSync { off, on };`, true)
  ]);
  describe("vng_render_opengl", [
    section("Compile products are owned together when useful", "OpenGLProgramRuntime groups production and analysis realizations of a linked program with capture support. production() supplies the normal compiled program. A renderer can use this helper or directly compile/run its own program; the helper does not dictate draw policy or a graphics-state baseline.", "include/vng/render_opengl/program_runtime.hpp"),
    section("A capture must retain its interpretation", "The enhanced path uses the analysis program and matching attachments, records item identities/observations, and exposes results with generated-source metadata. Follow FileMeshRenderer's analysis path to see the renderer supply IDs while drawing the same geometry. A screenshot answers appearance questions; IDs, depth and named observations answer which item/value produced a pixel.", "examples/file_mesh_renderer.cpp")
  ]);
  describe("vng_resources_opengl", [
    section("Build a ready owner, not a half-initialized object", "render::mesh_renderer_builder(device) collects mesh, program and optional image providers. build returns a ready opengl::MeshRenderer or a diagnostic. The owner stores realized resources and recipes. Tickets describe draw-time values; the engine does not force every custom renderer to use this builder or implementation.", "include/vng/resources_opengl/mesh_renderer.hpp"),
    section("Stage replacements, validate, then commit", "update(device) creates a transaction for mesh/program/albedo changes; commit validates and installs the coherent result. The transaction is tied to the owner's generation and is one-shot, so a stale transaction cannot overwrite newer resources. reload and per-resource reload helpers reuse retained providers; replace helpers accept replacements. Failed preparation does not intentionally leave a half-installed renderer.", "src/resources_opengl/mesh_renderer.cpp"),
    section("Do not confuse it with the editor's batch renderer", "This reusable MeshRenderer owns its own resource bundle and public ticket contract. BlueprintMeshRenderer is an application policy: the runtime shares a mesh program and creates one renderer per blueprint. Both use renderer identity, but their ownership and supported shader/instance contracts are different. Look at the concrete type before assuming arbitrary shader arguments can be supplied.", "tests/opengl/resource_owner_tests.cpp")
  ]);
  describe("vng_bloom_opengl", [
    section("Bloom starts with HDR light", "Render emission into a floating-point target, extract bright values, downsample/blur across a pyramid, then composite/tone-map into the output frame. Threshold chooses what contributes; strength changes added light; exposure changes display mapping. Bloom cannot recover intensity already clipped into an 8-bit image.", "include/vng/bloom_opengl/bloom.hpp"),
    section("The owner manages size-dependent resources", "bloom_builder(device).levels(...).build(extent) returns a renderer with the needed programs/targets. Resize/reload between frames, then apply to an output frame using the HDR color image. This is an explicit sequence, not a hidden render graph, and sun-specific texture/displacement policy does not belong in the generic bloom library.", "examples/glow.cpp", `auto bloom = render::bloom_builder(device).levels(6).build(initial);
if (!bloom) return fail(bloom.error());`, true)
  ]);
  describe("vng_rig_opengl", [
    section("Realize the binding once, update the pose often", "The skinned renderer uploads geometry, influences and shader resources from a neutral SkinBinding. A SkinnedDraw supplies pose and instance transform. Changing a pose updates the bone palette; it should not require rebuilding the mesh or moving rest positions on the CPU. Independent characters may share a binding while retaining different poses.", "include/vng/rig_opengl/skinned_mesh_renderer.hpp"),
    section("The neutral rig does not call its renderer", "make_skinned_mesh_renderer(device, binding) chooses the concrete implementation. Palette construction/validation comes from rig data; OpenGL buffers and draw submission stay here. Multiple pose tickets do not by themselves promise one GPU draw for all skins—the concrete renderer's batching strategy remains the authority.", "examples/rigging.cpp")
  ]);
  describe("vng_text_opengl", [
    section("Shape, cache, batch", "TextRenderer accepts text tickets and realizes shaped glyphs as textured geometry. Glyph atlases and associated GPU resources are persistent renderer-owned state; repeated labels should reuse them instead of rasterizing/uploading every frame. Font/size differences affect cache entries and batching, not the logical definition of a button.", "include/vng/text_opengl/text_renderer.hpp"),
    section("Clipping and blending are draw concerns", "Tickets carry layout/presentation information such as position and clipping. The renderer handles the texture/state needed to display glyph coverage. It does not own UI text editing, focus, caret movement or clipboard policy. Read the standalone text example before investigating widget behavior in ui.cpp.", "examples/text.cpp")
  ]);
  describe("vng_ui_opengl", [
    section("Render a Screen without taking ownership of it", "The UI renderer consumes neutral drawing data for widget shapes and text. It owns its OpenGL realization and a text renderer, not the Screen's widget state. Input/layout can therefore be tested in vng_ui without a GL context, while pixel/blending behavior is tested in the integration.", "include/vng/ui_opengl/ui_renderer.hpp"),
    section("Editor panels reuse the same rendering path", "Panels build/update widgets and issue authoring actions; the backend draws the resulting DrawList into the frame. Scissoring, batched geometry and atlas reuse belong here. Reducing unnecessary scene serialization is an editor update-boundary issue, not something this renderer should solve by learning about documents.", "src/ui_opengl/ui_renderer.cpp")
  ]);

  describe("vng_earth_assets", [
    section("An offline generator, not a renderer", "make_mesh(CloudSettings) returns a complete Earth document with position, normal, color and emission fields: +Y is north, Greenwich faces +Z and the shorelines are deliberately simplified illustrations. is_earth recognizes a generated asset by its generator metadata rather than by a display name, so renaming a blueprint never changes its behavior. make_savannah_variant recolors land only and keeps geometry, clouds and custom fields, which is how earth_savannah.vmesh was produced.", "examples/support/earth_assets.hpp"),
    section("Cloud formations are editable data inside the mesh", ["Cloud settings (coverage, puff and spiral size, altitude, relief, edge scatter, visibility) are written into the document, and cloud_formations lists stable formation identities with names and longitude/latitude placement. move_cloud, rotate_cloud, add_cloud and remove_cloud return new documents that keep hand edits and neighboring formations; rebuild_clouds regenerates from settings and a legacy mesh needs one rebuild before it owns formations.", "The editor's blueprint controls wrap these calls in MeshDraftEdit requests. Drafts, undo and preview delivery stay in the editor project; this library only transforms documents."], "examples/support/earth_clouds.hpp", `struct CloudFormation {
    vng::u32 id;
    std::string name;
    vng::Vec2 location;
};`, true)
  ]);
  describe("vng_editor_project", [
    section("EditingSession is the authoring authority", "EditingSession owns State, active gesture, undo/redo, clipboard and SceneFile persistence. state() is a const view of the authored model; typed operations make edits and produce EditNotice. viewport() exposes separate private viewing state. The session does not include UI widget handles, IPC endpoints or a Device.", "examples/editor/editing_session.hpp"),
    section("Blueprints, instances and drafts are explicit", "Document stores shared blueprints, scene instances, the authored camera and timeline. Mesh drafts are separate from applied blueprint geometry: editing a draft does not silently alter scene instances; Apply publishes it. Deleting an instance leaves its blueprint. A region blueprint supplies a starting shape, while each region instance owns its editable boundary.", "examples/editor/project.hpp"),
    section("A gesture has one history meaning", "Tool begin/update/commit/cancel calls describe one user action even if it generated many mouse samples. Changes carry affected identities rather than requiring a whole-document diff. DocumentPatch captures the corresponding values for replay/undo or worker delivery; structural operations can require a snapshot. Saved-content identity tracks authored changes, not private camera navigation.", "examples/editor/document_patch.hpp"),
    section("Narrow changes survive the delivery boundary", "DocumentChanges records dirty instance properties, mesh vertex IDs, region points, markers and structural flags. PreviewUpdates coalesces the latest absolute values while one authored revision awaits acknowledgment. Private camera/selection requests use a separate replaceable sequence, so orbiting should not resend document contents or rebuild the timeline inspector.", "examples/editor/preview_updates.hpp")
  ]);
  describe("vng_example_support", [
    section("Keep runnable examples about the feature", "GlfwOpenGLSession handles the concrete window/context/device pairing. WindowLoop handles repeated event polling, valid framebuffer extents and optional frame limits. Shared options and diagnostic printing keep file_mesh_direct.cpp focused on schema, shaders and drawing rather than boilerplate.", "examples/support/glfw_opengl_session.hpp"),
    section("A helper is not a mandatory engine shell", "Applications may own the window and device directly or choose another window backend. This helper commits to GLFW + OpenGL and therefore belongs in examples/support. Its lifetime ordering is valuable; making all engine types depend on it would invert the intended boundary.", "examples/support/window_loop.hpp")
  ]);
  describe("vng_example_presentation", [
    section("Display completed images", "DisplaySurface uploads/displays image results and supports capture output with explicit OpenGL resources. The editor uses image presentation after a worker frame arrives; standalone examples use the same helper for rendered targets. Presentation does not deserialize the document, run effects or decide which instance is selected.", "examples/support/presentation.hpp"),
    section("Presentation metadata belongs beside the pixels", "A displayed frame can lag behind the newest camera request. The editor keeps a matching PresentedView so overlays and picking refer to the image actually on screen. DisplaySurface handles image realization; PreviewMailbox and the editor own ordering/revision acceptance. Those jobs should not be folded into a generic texture wrapper.", "examples/editor/presented_view.hpp")
  ]);
  describe("vng_sun_support", [
    section("Effect behavior is ordinary project C++", "sun_shaders.cpp builds surface/displacement expressions in the DSL; sun_renderer.cpp owns effect rendering resources and draw policy; sun_softening.cpp implements artistic image treatment. Time and effect settings enter through explicit draw data/arguments rather than rerunning shader-building lambdas every frame.", "examples/support/sun_shaders.cpp"),
    section("Reuse the effect without importing editor policy", "The sun demo and scene runtime reuse this effect support. Editor controls describe tunable values and update the worker's state; the effect remains responsible for how those values are rendered. Editing its appearance should not require changes to generic timeline sampling, document transport or bloom extraction.", "examples/support/sun_renderer.hpp")
  ]);
  describe("vng_editor_runtime", [
    section("Own GPU realization, borrow scene input", "Runtime owns its shared mesh program, per-blueprint MeshResource entries, effect renderers, HDR targets, softening, bloom and display resources. Operations receive a Device and State; Runtime does not become the authoritative editor document. Both the worker and standalone cinematic demos can reuse it.", "examples/editor/runtime.hpp"),
    section("Batch by blueprint, split only for real draw differences", "For each visible mesh blueprint, Runtime collects instance tickets and submits one batch to BlueprintMeshRenderer. The renderer owns one GPU mesh and reusable solid/wireframe instance buffers, borrowing the runtime's stable shared Program. Mixed solid/wireframe state can require two GPU draws; 'one renderer call' and 'one GPU draw' are not identical promises.", "examples/editor/blueprint_mesh_renderer.hpp"),
    section("Keep shared lifetime explicit", "The shared program is declared so it outlives the renderer entries that borrow it. Document patches invalidate affected resources; changing an instance transform updates instance data, not shared mesh geometry. Geometry/blueprint structural changes may require rebuilding their realization. Per-effect rendering remains its own policy instead of being forced into the mesh batch contract.", "tests/editor/mesh_batch_tests.cpp")
  ]);
  describe("vng_editor_worker", [
    section("One context-owning loop, separate from the UI", "editor_worker.cpp owns WorkerEndpoint, local scene/view state, a window/context, Device and Runtime. It receives updates, samples playback, invokes effect behavior and renders on its context-owning thread. This is not a separate simulation-thread architecture hiding behind the IPC transport.", "examples/editor_worker.cpp"),
    section("Authored state and viewing state arrive differently", "A document patch or snapshot changes the worker's authored replica and is acknowledged. A ViewportRequest supplies newest absolute camera/playhead/inspection state. Completed frames include generation/revision/view metadata, so UI presentation can reject stale results and pair pixels with their actual camera. The worker never calls a UI-process widget callback.", "examples/editor/viewport_session.hpp"),
    section("Independent Play is a separate presentation mode", "Independent Play gives the worker its own native presentation window for full-speed output. Debug communication can remain enabled or be disabled. An editable viewport popped out by the UI is different: it still displays worker output and uses the existing UI authoring session. Neither mode silently duplicates the document authority.", "tests/editor/worker_tests.cpp")
  ]);
  describe("vng_editor_ui", [
    section("Panels and dialogs are retained widgets over a const State", "InspectorPanel, TimelinePanel and BlueprintMeshPanel own their vng::ui widgets and ID lookups and borrow document data at sync(), so a selection or tab change never copies the scene. Each panel exposes polled actions (clicked, submitted, changed) that the app reads once per frame after Screen::update(); the panel itself never touches EditingSession history or the worker. FileDialog is one picker parameterized by a FileDialogSpec; ImportDialog, OpenSceneDialog and SaveDialog only choose words and accepted extensions.", "examples/editor/file_dialog.hpp"),
    section("Gizmo tools are gesture math plus visuals, not document owners", "TranslationTool, RotationTool and ScaleTool draw handles and turn pointer motion into typed deltas from a captured camera snapshot; TransformGesture reuses the same visuals for keyboard-driven modal transforms. ComponentTransform, CageTool, RegionEditor and SurfaceMoveTool follow the same shape: capture on press, report an action struct on release, and let the host decide which EditingSession gesture to begin, update or commit. Because nothing here needs a GL context, vng_editor_ui_tests drives every panel and tool headlessly through Screen::update() with synthetic input.", "examples/editor/transform_gesture.hpp"),
  ]);
  describe("vng_editor_app", [
    section("app.cpp is the composition root", "The app owns EditingSession, retained Screens/panels, ViewportInteraction, PreviewSession, PreviewUpdates, PreviewMailbox and display/window resources as explicit collaborators. It polls events, updates UI/tools, routes notices, polls worker output and renders the UI. The conceptual ownership groups are not extra manager classes that callers must construct.", "examples/editor/app.cpp"),
    section("Panels issue intent; tools operate on the session", "Scene/Keyframe/Properties panels read session state and invoke operations. ViewportInteraction arbitrates camera navigation, picking, box selection and transforms. Blueprint manipulation descriptions supply allowed/custom gizmos; multiple selection exposes the common set. Mesh and region component transforms share tool behavior without turning every vertex into a scene instance.", "examples/editor/blueprint_gizmos.hpp"),
    section("UI placement is not a document dependency", "editor_layout.hpp places top toolbars, viewport, sidebar tabs and timeline. Screen retains focus, hover and scroll state. The editable pop-out shares the same session/panels; independent Play belongs to the worker. Changing toolbar placement should not change serialization, worker protocol or renderer ownership.", "examples/editor/editor_layout.hpp"),
    section("Test interactions through the real application", "Automation supplies native input and read-only observations used by editor end-to-end tests. Workflows exercise importing, selecting, editing, undo and worker presentation rather than only calling model methods. CPU session/tool tests isolate invariants; native UI tests catch hit-testing, focus, layout and latency-boundary regressions.", "examples/editor/automation.hpp")
  ]);
  describe("vng_editor_demo", [
    section("Start here, then follow one subsystem", "examples/editor.cpp parses startup options and calls editor_example::run. app.hpp is the application entry contract. The executable links the app library; it does not link the worker executable into the same process. CMake orders the worker build so the UI can launch it.", "examples/editor.cpp"),
    section("Choose a reading path rather than reading everything", "For an edit, follow tool → EditingSession → DocumentChanges/Patch → PreviewUpdates → worker. For pixels, follow worker → Runtime → BlueprintMeshRenderer/effect → Commands → OpenGL. For a button, follow panel → Screen update → polled result → session operation. These paths explain how the target dependency map becomes an actual interaction.", "examples/editor/app.hpp")
  ]);

  // The lightweight identity/alias headers are useful alongside the full contract.
  byTarget.get("vng_render").links.push(
    { label: "include/vng/render/backend.hpp", href: "../../include/vng/render/backend.hpp" },
    { label: "include/vng/opengl/renderer.hpp", href: "../../include/vng/opengl/renderer.hpp" });
})();
