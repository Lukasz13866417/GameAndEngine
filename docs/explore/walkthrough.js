/* A reading path through real C++ abstractions, not another target inventory. */
(() => {
  "use strict";
  const type = (id, symbol, summary, explanation, source, code, owns, borrows, related = [], exact = false) => ({
    id: `walk-${id}`, title: symbol, symbol, kind: "C++ type", summary,
    description: [explanation],
    sections: [{ title: "API in context", paragraphs: [], source, code, exact }],
    ownership: { owns, borrows },
    links: [{ label: source, href: `../../${source}` }], related
  });
  const group = (id, title, summary, description, children) => ({
    id, title, kind: "Reading path", summary, description: [description], children
  });
  window.VNG_GUIDE.push({
    id: "code-walkthrough", title: "Walk through the actual C++", kind: "Source walkthrough",
    summary: "Start with a vertex, follow a draw, then follow an editor change into the worker.",
    description: ["These blocks name real classes and templates. Read in order for the complete path, or open one type to learn its purpose, API and lifetime. Excerpts are short on purpose; the source link is the full declaration or runnable example.", "The nesting below is a reading sequence, not inheritance or ownership. Record does not own a shader; the editor document does not own a GPU renderer. Each card says explicitly what it owns and borrows, and related links show collaboration."],
    boundary: "Usage sketches omit surrounding setup; marked source excerpts are checked against the repository. Most factories return expected—check errors before dereferencing. This guide describes implemented OpenGL realization, not an already-implemented second graphics backend.",
    related: ["code-components", "code-ownership", "draw-a-frame"],
    children: [
      group("walk-cpu", "1. Describe and own CPU geometry", "Semantic meaning becomes packed records, contiguous streams and indexed mesh data.",
        "These values can be created and tested before opening a window. Their storage is not shared automatically with the GPU.", [
        type("record", "vng::gfx::Record<Fields...>", "One vertex-like value with explicitly calculated byte layout.",
          "A semantic's C++ type identifies meaning; its value_type describes the logical value. as<Tag, Codec> selects encoding for one occurrence. Record::has checks presence, get decodes, and set encodes. Position and Color are application tags, not engine-enforced field names. The calculated stride does not depend on inheritance or native struct padding.",
          "examples/file_mesh_types.hpp", `using Vertex = vng::gfx::Record<
    Position,
    vng::gfx::as<Color, vng::gfx::unorm8x4>>;`,
          ["Its packed field bytes; the schema and codecs are compile-time types."], ["No external record storage."], ["walk-vertex-stream", "code-gfx"], true),
        type("vertex-stream", "vng::gfx::VertexStream<Record>", "An owning, contiguous CPU array of typed records.",
          "Indexing gives a record reference; bytes() exposes a borrowed upload span without making a copy. Resizing can invalidate those references/spans. This object is not a GPU buffer, and changing it does not implicitly upload anything. A mesh may combine several streams for separate attribute groups.",
          "include/vng/gfx/vertex_stream.hpp", `vng::gfx::VertexStream<Vertex> vertices(3);
vertices[0].set(Position{}, {-0.5F, -0.5F, 0.0F});
vertices[0].set(Color{}, {1.0F, 0.0F, 0.0F, 1.0F});
auto bytes = vertices.bytes();`,
          ["std::vector<Record> storage."], ["bytes() callers borrow storage only until invalidation."], ["walk-record", "walk-mesh"]),
        type("mesh", "vng::gfx::Mesh<RecordTypes...>", "Geometry: streams, faces and optional explicit edges.",
          "vertices() accesses the single stream, or vertices<Record>() selects among multiple typed streams. faces() stores indexed topology. validate() checks stream counts and indices. The example loads a typed CPU mesh through a string-to-semantic schema. A scene instance's transform and identity do not belong to this geometry object.",
          "examples/file_mesh_direct.cpp", `const std::filesystem::path mesh_path{VNG_EXAMPLE_MESH_PATH};
auto cpu_mesh = vmesh::load(mesh_path, vertex_schema);
if (!cpu_mesh) {
    return example::fail(cpu_mesh.error());
}`, ["Typed CPU streams, indexed faces, authored edges and mesh information."], ["No Device, window or renderer."], ["walk-gpu-mesh", "walk-document", "code-content"], true)
      ]),
      group("walk-shader", "2. Record a typed shader program", "Expressions are temporary handles; completed stages own the recorded calculation.",
        "Execute C++ to build the shader once. Later draw calls supply values to that recorded program; they do not replay the shader-building lambda.", [
        type("expr", "vng::dsl::Expr<T>", "A typed reference to one value in the shader being built.",
          "Float, Float3 and Float4x4 are aliases. Arithmetic appends operations inferred from the operands' builder. Copying an Expr copies its builder pointer/value ID, not a calculation or variable assignment. Expressions cannot become host bool and must not escape their builder lifetime. Literal operands record constants; runtime inputs should become typed shader arguments.",
          "include/vng/dsl/expr.hpp", `// Inside a shader lambda:
vng::dsl::Float3 p = stage.input(Position{});
auto translated = p + offset; // offset is another Float3
auto clip = stage.camera().project(translated);`,
          ["Only a FunctionBuilder pointer and ValueId."], ["The currently recording FunctionBuilder; it must still exist."], ["walk-stage-context", "walk-ir", "walk-arguments"]),
        type("stage-context", "vng::shader::StageContext<...>", "The shader lambda's stage parameter: inputs, outputs, constants and built-ins.",
          "The factory supplies stage. input(Tag{}) returns a semantic expression; camera() exposes the camera parameter; output(field<Tag>(...)) creates the complete output record. observe names a diagnostic value without turning it into ordinary fragment output. Explicit constants are snapshots, not live references to CPU variables.",
          "examples/file_mesh_renderer.cpp", `auto fragment = vng::shader::fragment<FragmentIn, FragmentOut>(
    "file_mesh_fragment",
    [](auto& stage, vng::dsl::Float brightness) {
        const auto source = stage.input(Color{});
        const auto color = vng::dsl::vec4(source.xyz() * brightness, source.w());
        stage.observe(SurfaceColor{}, color);
        return stage.output(
            field<vng::shader::Color<0>>(color));
    });`,
          ["A lightweight access handle; not the IR tables themselves."], ["The factory's FunctionBuilder, used by expressions during this recording lifetime."], ["walk-expr", "walk-stage", "code-analysis"], true),
        type("ir", "vng::shader::FunctionBuilder / ModuleIR", "The builder creates tables; the finished IR owns typed operations and regions.",
          "Values refer to their producer operation by ID; operations reference operands, result, nested regions, payload, effect and source origin. One producer may have many uses. This is inspectable computation, not GLSL text. Normally use stage factories rather than constructing IR tables manually; validation handles internal consistency and cross-builder poison diagnostics.",
          "include/vng/shader/ir.hpp", `struct ValueNode {
    TypeId type;
    OperationId producer;
};`,
          ["FunctionBuilder's in-progress module; ModuleIR's types, values, operations, regions and metadata."], ["Expr handles borrow the builder only while recording."], ["walk-stage", "code-shader"], true),
        type("stage", "vng::shader::TypedShaderStage<Kind, Args...>", "A completed vertex or fragment stage, with its CPU argument signature intact.",
          "The vertex/fragment factory returns this owning result after recording and validation. ShaderStage inside it owns ModuleIR. kind(), ir(), dump_ir() and dump_interface() expose inspection; untyped() borrows the underlying stage, while release_untyped() explicitly moves it out. The lambda itself is not retained for later backends.",
          "include/vng/shader/stage.hpp", `[[nodiscard]] const ModuleIR& ir() const noexcept { return stage_.ir(); }
[[nodiscard]] std::string dump_ir() const { return stage_.dump_ir(); }
[[nodiscard]] std::string dump_interface() const { return stage_.dump_interface(); }`,
          ["A ShaderStage containing completed ModuleIR."], ["ir()/untyped() callers borrow from this stage."], ["walk-ir", "walk-program"], true),
        type("program", "vng::shader::TypedGraphicsProgram<Args...>", "A linked, backend-neutral vertex/fragment pair.",
          "link checks interface compatibility and preserves the typed argument list. By default vertex arguments come before fragment arguments; equal types do not imply shared values. GraphicsProgram owns both stages. compile_program(device, program) later chooses a backend realization without rerunning their lambdas. Keep this neutral program for inspection or reconstruction recipes.",
          "examples/file_mesh_direct.cpp", `auto shader_program = vng::shader::link(
    std::move(*vertex), std::move(*fragment));
if (!shader_program) {
    return example::fail(shader_program.error());
}`, ["A GraphicsProgram, which owns the linked stages."], ["No graphics context; compilation receives a Device separately."], ["walk-gpu-program", "walk-arguments", "code-glsl"], true),
        type("arguments", "vng::shader::ArgumentPack<Ts...>", "Short-lived logical CPU argument words for one backend call.",
          "The typed run API requires the exact CPU signature and constructs the pack internally. Scalar bits, vector components, matrix columns and decoded record fields become logical words. ArgumentView borrows those words. This is not the packed byte ABI of VertexStream; callers usually just pass their ordinary model matrix/brightness values.",
          "include/vng/shader/arguments.hpp", `struct ArgumentView final {
    std::type_index type{typeid(void)};
    std::span<const u32> words;
};`,
          ["Stack-owned word arrays for the supplied logical values."], ["Returned views are consumed before the pack is destroyed."], ["walk-program", "walk-commands"], true)
      ]),
      group("walk-draw", "3. Realize resources and submit a draw", "The neutral contracts meet a concrete backend; resource lifetime becomes explicit.",
        "These are collaborating objects, not a single owner chain. The application keeps the window/context alive, owns a Device and persistent resources, and creates a short-lived Frame for submission.", [
        type("backend", "vng::opengl::Backend", "A lightweight compile-time identity, not a context or singleton.",
          "Backend names Device and Frame types. render::Backend checks the identity contract; backend_t<T> reads it from a bound type. Device, Frame, Commands and renderers expose backend_type. SameBackend catches incompatible backend types, not mismatched OpenGL contexts. The identity header need not include GL functions or GLFW.",
          "include/vng/opengl/backend.hpp", `struct Backend final {
    using device_type = Device;
    using frame_type = Frame;
};`, ["No runtime state."], ["No device instance; aliases name types only."], ["walk-renderer", "walk-device"], true),
        type("renderer", "vng::render::Renderer<Ticket, Backend>", "A base identifying a drawing policy; the subclass owns the actual implementation.",
          "The backend argument is mandatory. opengl::Renderer<Ticket> is the short alias. Derive publicly and implement render(frame, view, span<const Ticket>); own resources and decide batching/state in that subclass. RendererFor checks backend compatibility plus a real callable batch API. The empty base neither routes calls nor makes backend-specific code portable.",
          "include/vng/render/renderer.hpp", `template<vng::render::Backend B>
class ModelRenderer : public vng::render::Renderer<ModelDraw, B> {
public:
    Result render(typename B::frame_type&,
                  const vng::render::RenderView&,
                  std::span<const ModelDraw>);
}; // ModelDraw and Result are application types here.`,
          ["Nothing in the base; subclasses declare persistent resources explicitly."], ["Frames, views and ticket spans arrive for a render call."], ["walk-backend", "walk-blueprint-renderer", "code-render"]),
        type("device", "vng::opengl::Device", "The validated access point to one OpenGL context/thread.",
          "create(currentContextAccess, options) loads procedures and validates support. Device does not create a window. The GLFW/OpenGL bridge supplies current-context access; another window integration could do so too. Creation, use and destruction of its resources require a compatible current context. Error diagnostics are values, not a substitute for the lifetime rule.",
          "include/vng/opengl/device.hpp", `// window is a glfw_opengl::Window; keep it alive longer than device.
auto access = window.make_current();
if (!access) return fail(access.error());
auto device = vng::opengl::Device::create(*access);
if (!device) return fail(device.error());`,
          ["Device/context validation and diagnostic state, not the native window."], ["CurrentContextAccess from the window/graphics integration."], ["walk-frame", "walk-gpu-mesh", "code-glfw-opengl"]),
        type("gpu-program", "vng::opengl::TypedProgram<Args...>", "A compiled native program plus the original typed submission signature.",
          "compile_program's device-selected implementation emits GLSL, compiles driver shaders and links Program. The wrapper preserves CPU argument types so run cannot accidentally pass the wrong list. Program owns the GL object and generated/interface metadata. This is not a persistent graphics-state preset and not GL_PROGRAM_PIPELINE.",
          "examples/file_mesh_direct.cpp", `auto program = vng::render::compile_program(app->device(), *shader_program);
if (!program) {
    return example::fail(program.error());
}`, ["The compiled Program and its generated metadata."], ["Compatible owning context for operations and destruction."], ["walk-program", "walk-commands"], true),
        type("frame", "vng::opengl::Frame", "Short-lived active rendering scope, with target and output policy.",
          "render::begin_frame dispatches through Device. FrameDesc chooses extent, clear values and output encoding. render_context() returns Commands, and end() closes the scope. Commands handles share the frame's state and become invalid when it ends. Keep target attachments alive; resize/reload them between frames.",
          "include/vng/opengl/frame.hpp", `auto frame = vng::render::begin_frame(device, description);
if (!frame) return fail(frame.error());
auto commands = frame->render_context();
// Submit draws here, then check frame->end().`,
          ["The active frame access/state and scoped target configuration."], ["Device/context and target resources supplied for the frame."], ["walk-commands", "walk-device"]),
        type("commands", "vng::opengl::Commands", "A borrowed interface for changing live state and issuing draws.",
          "graphics_state().set modifies individual settings; run selects program and current CPU arguments; view supplies camera data; draw submits geometry. Copies share one frame state rather than recording independent command lists. run does not reset culling/depth. Raw GL can disturb cached state; restore is the explicit recovery path.",
          "examples/file_mesh_direct.cpp", `if (auto bound = commands.run(*program, model, brightness); !bound) {
    return example::fail(bound.error());
}
if (auto viewed = commands.view(*view); !viewed) {
    return example::fail(viewed.error());
}
if (auto drawn = commands.draw(*gpu_mesh); !drawn) {
    return example::fail(drawn.error());
}`, ["An access handle, not a second GPU resource owner."], ["Active Frame state, selected program, geometry and per-call data."], ["walk-frame", "walk-gpu-mesh", "walk-arguments", "code-render"], true),
        type("gpu-mesh", "vng::opengl::GpuMesh<RecordTypes...>", "GPU geometry and prepared semantic vertex-input bindings.",
          "upload_mesh explicitly realizes CPU geometry. prepare_vertex_input(device, program) checks the shader contract and prepares matching VAO state; callers do not hand-map numeric attributes. The GPU mesh owns its geometry buffers, not the CPU mesh or scene instances. Its resources remain valid only in their device/context domain.",
          "examples/file_mesh_direct.cpp", `auto gpu_mesh = vng::opengl::upload_mesh(app->device(), *cpu_mesh);
if (!gpu_mesh) {
    return example::fail(gpu_mesh.error());
}`, ["GPU vertex/index buffers and prepared vertex-input realization."], ["Program metadata during preparation; compatible Device/context."], ["walk-mesh", "walk-instance-buffer", "walk-commands"], true),
        type("instance-buffer", "vng::opengl::InstanceBuffer<Record>", "Reusable GPU records for values that vary per instance.",
          "update(device, records) uploads a batch and grows capacity geometrically when needed. Geometry stays in the shared GpuMesh. draw(mesh, instanceBuffer) uses the instance record's semantics with per-instance delivery. The editor stores model columns/lighting/brightness here, allowing value differences without separate geometry draws.",
          "include/vng/opengl/instance_buffer.hpp", `vng::opengl::InstanceBuffer<Instance> instances;
auto updated = instances.update(device, std::span<const Instance>{records});
if (!updated) return updated;
return commands.draw(mesh, instances);`,
          ["Reusable GPU buffer allocation, capacity and active size."], ["CPU records during update, Device/context, mesh/program during drawing."], ["walk-gpu-mesh", "walk-blueprint-renderer"])
      ]),
      group("walk-authoring", "4. Make an authored editor change", "One session owns editable content; document and viewing state have different revision rules.",
        "Unlike the generic engine types above, these live in examples/editor and express this application's policy. The session is the mutation authority; UI controls and tools ask it to perform operations.", [
        type("session", "editor_example::EditingSession", "The owner of authoring transactions, undo/redo, clipboard and scene persistence.",
          "state() gives const read access; typed operations mutate authored content. begin/update/commit/cancel treats many mouse samples as one history action. take_changes() emits notices for the app to display/deliver. viewport() allows independent private navigation; the session does not know about widgets, IPC or GPU resources.",
          "examples/editor/editing_session.hpp", `auto begun = session.begin_move(instance_id, selected_ids);
if (!begun) return begun;
auto moved = session.move(new_primary_position);
if (!moved) { (void)session.cancel(); return std::unexpected(moved.error()); }
auto committed = session.commit();
// The application consumes session.take_changes() for delivery.`,
          ["State, active transaction, history, clipboard, SceneFile and saved-content identity."], ["No long-lived UI/preview/Device dependency."], ["walk-state", "walk-changes", "walk-updates"]),
        type("state", "editor_example::State", "Two sibling values assembled for the editor: Document and ViewportState.",
          "This type is deliberately small. Document stores authored content; ViewportState stores private inspection/navigation/playhead state. A combined State is convenient for local operations, but transport does not treat it as one global revision that must be serialized after every camera move.",
          "examples/editor/project.hpp", `struct State {
    Document document;
    ViewportState viewport{};
};`, ["Document and ViewportState as sibling members."], ["No GPU realization."], ["walk-document", "walk-viewport-state", "walk-runtime"], true),
        type("document", "editor_example::Document", "Authored blueprints, instances, animation camera, timeline and mesh drafts.",
          "Mesh assets own applied geometry. mesh_drafts contains private working geometry that scene instances do not resolve through; Apply publishes a draft. Deleting instances does not delete blueprints. revision orders authored changes, not selection/orbiting. State assembly is owned by EditingSession in the UI and separately reconstructed in the worker.",
          "examples/editor/project.hpp", `// Some actual members; see project.hpp for the complete declaration.
std::vector<MeshBlueprint> mesh_assets{};
std::map<BlueprintId, vng::editor::EditableMesh> mesh_drafts{};
std::vector<SceneInstance> instances;`,
          ["Authored CPU geometry/assets, instance records, timeline, camera/environment/bounds and drafts."], ["Instances refer to blueprints by ID, not owning pointers."], ["walk-instance", "walk-session", "walk-patch"]),
        type("instance", "editor_example::SceneInstance", "One placed use of a blueprint, with its own settings and transform.",
          "blueprint is an identity in the document catalog. transform changes the instance without altering shared mesh geometry. The settings variant distinguishes mesh/sun/region policy; each region instance carries its own editable boundary. A vertex in a region is not another SceneInstance. Deleting the instance leaves the reusable blueprint available.",
          "examples/editor/project.hpp", `struct SceneInstance {
    vng::u32 id{};
    BlueprintId blueprint{BlueprintId::mesh};
    std::string name;
    std::variant<MeshSettings, SunSettings, RegionSettings> settings;
    InstanceTransform transform{};
    friend bool operator==(const SceneInstance&, const SceneInstance&) = default;
};`, ["Instance name/settings/transform and numeric IDs."], ["Blueprint geometry is resolved through the Document catalog."], ["walk-document", "walk-blueprint-renderer"], true),
        type("viewport-state", "editor_example::ViewportState", "Private viewing intent with its own sequence, not authored scene content.",
          "It records inspected mesh, active object/vertex, playhead, pause state and editor camera. pilot_camera selects editing the authored animation camera; ordinary navigation remains private. The active inspection target is not the entire UI multi-selection set. Newer absolute requests may replace older ones without losing an authored operation.",
          "examples/editor/project.hpp", `// These are separate counters with different meanings:
auto authored_revision = session.state().document.revision;
auto view_sequence = session.viewport().sequence;`,
          ["Camera/navigation/inspection/playhead values and a viewport sequence."], ["Object/blueprint IDs resolve against the current Document."], ["walk-state", "walk-view-request"]),
        type("changes", "editor_example::DocumentChanges", "Small identities of what changed, not another copy of the document.",
          "It records affected property targets, vertices per blueprint, region points, markers and structural flags. merge combines dirty identities without scanning unrelated meshes. A full flag admits structural changes where a sparse update is inappropriate. This descriptor contains no captured values, serialization or GPU objects; DocumentPatch is the value payload.",
          "examples/editor/document_changes.hpp", `DocumentChanges changes;
changes.vertices[blueprint_id].insert(vertex_id);
changes.properties.insert({instance_id, "position"});
pending.merge(changes);`,
          ["Dirty-ID sets/maps and full/duration/world-bounds flags."], ["No borrowed scene storage."], ["walk-patch", "walk-updates"]),
        type("patch", "editor_example::DocumentPatch", "A revision-bounded payload of changed authored values.",
          "capture_patch reads values described by DocumentChanges. apply_patch validates and applies them; encode/decode handles transport. Sparse vertex/region edits preserve exact indices, so topology changes need an appropriate replacement. PropertyPatch can replace/remove one property's track without rebuilding unrelated tracks. History and worker delivery reuse explicit patch information.",
          "examples/editor/document_patch.hpp", `[[nodiscard]] vng::content::Result<DocumentPatch> capture_patch(vng::u64 base, const State&, const DocumentChanges&);
[[nodiscard]] vng::content::Result<void> apply_patch(State&, const DocumentPatch&);
[[nodiscard]] vng::content::Result<std::string> encode_patch(const DocumentPatch&);
[[nodiscard]] vng::content::Result<DocumentPatch> decode_patch(std::string_view);`,
          ["Base/new revisions and captured property, vertex, region, marker/bounds values."], ["State only during capture/apply; payload owns the captured data."], ["walk-changes", "walk-updates", "walk-worker-endpoint"], true)
      ]),
      group("walk-preview", "5. Deliver, render and present the result", "The worker owns GPU realization; the UI accepts matching completed images.",
        "Two lanes cross the process boundary: ordered authored updates and replaceable absolute viewing requests. No C++ object pointer or GL handle is the scene protocol.", [
        type("updates", "editor_example::PreviewUpdates", "Coalesce authored changes while waiting for the worker's acknowledgment.",
          "Each worker generation has a single pending authored revision slot. changed(changes) records affected targets; next(generation, state) creates the next message when allowed; acknowledge releases the slot. Repeated mouse samples become latest absolute values rather than a queue of full snapshots. Failure/reconnect can reset to a full snapshot safely.",
          "examples/editor/preview_updates.hpp", `updates.changed(notice.changes);
auto message = updates.next(generation, session.state());
if (!message) return fail(message.error());
if (*message) {
    auto sent = preview.send(**message, generation);
    if (!sent) updates.reset(generation);
}`, ["Per-generation acknowledgment/coalescing state and dirty identities."], ["State only while making a message; transport owned elsewhere."], ["walk-changes", "walk-patch", "walk-preview-session"]),
        type("view-request", "editor_example::ViewportRequest / ViewportInbox", "An absolute newest view, with a required authored revision.",
          "ViewportRequest carries ViewportState separately from DocumentPatch. The worker's ViewportInbox retains the newest request until its required document revision exists, then validates/resolves selection against that document. send_latest is safe for this absolute intent, not for incremental edits that all must execute.",
          "examples/editor/viewport_session.hpp", `struct ViewportRequest {
    vng::u64 required_document_revision{};
    ViewportState view;
};`, ["A request's value snapshot; inbox keeps at most its pending newest request."], ["Worker State during apply; no UI-process State pointer."], ["walk-viewport-state", "walk-preview-session", "walk-worker-endpoint"], true),
        type("preview-session", "vng::editor::preview::PreviewSession", "UI-side ownership of worker/build processes and communication.",
          "create(config) prepares the connection. poll() observes events without waiting for a render/build; send queues normal messages, while send_latest replaces pending absolute-view messages. Build/reload is process orchestration, not shader lambda replay. Trusted project code runs with user permissions: process isolation is not a security sandbox.",
          "include/vng/editor/preview.hpp", `Result<void> send(std::string_view message, std::uint64_t generation = 0);`,
          ["Child/build process and transport handles behind its implementation."], ["Application supplies serialized messages; authoring remains in EditingSession."], ["walk-updates", "walk-worker-endpoint", "walk-mailbox"], true),
        type("worker-endpoint", "vng::editor::preview::WorkerEndpoint", "Worker-side control messages and completed image publication.",
          "receive reads queued control messages; try_publish exposes a completed RGBA image and FrameInfo, returning false rather than waiting if the UI is reading. The worker retains only the newest completed frame for retry. editor_worker.cpp—not this transport class—decodes the scene protocol, applies updates, samples playback and calls Runtime. The context-owning loop polls asynchronous GPU transfers between input/render steps; there is no hidden game-logic thread.",
          "include/vng/editor/preview.hpp", `Result<bool> try_publish(FrameInfo info, std::span<const std::byte> rgba);`,
          ["Worker-side transport/mapping handles."], ["Pixel span during publication; application decides frame metadata and rendering."], ["walk-runtime", "walk-view-request", "walk-mailbox"], true),
        type("runtime", "editor_example::Runtime", "The scene's GPU realization, reusable by editor worker and cinematic demos.",
          "create prepares resources. RenderRequest borrows State and supplies camera/extent/time explicitly; annotations asks for editor-preview decoration that independent Play never draws. update_positions applies narrow vertex changes; render_frame renders without forcing readback. Live preview uses queue_readback/poll_readback with a bounded OpenGL staging queue; synchronous readback remains for screenshots/evidence. present is separate. Runtime owns programs/mesh realizations/effect and postprocess resources, but never becomes the authoritative authoring session.",
          "examples/editor/runtime.hpp", `struct RenderRequest {
    const State& state;
    vng::gfx::Camera camera;
    vng::Extent2D extent;
    vng::f32 time;
    bool diagnostic{};
    bool annotations{}; // Editor preview only; independent play remains undecorated.
};`,
          ["Shared mesh program, blueprint resources, sun/HDR/bloom/presentation resources via Impl."], ["Device and immutable State inputs for each operation."], ["walk-blueprint-renderer", "walk-state", "walk-device"], true),
        type("blueprint-renderer", "editor_example::BlueprintMeshRenderer", "One mesh realization and batched tickets for one blueprint.",
          "Runtime gathers visible instances per blueprint and calls render once for that batch. The renderer holds one GPU mesh and solid/wireframe InstanceBuffer batches; ordinary transform/lighting differences become record values, not new draw calls. Mixed raster state can need two GPU draws. Its shared Program is explicitly borrowed from Runtime and must outlive it.",
          "examples/editor/blueprint_mesh_renderer.hpp", `BlueprintMeshRenderer(GpuMesh mesh,const vng::opengl::Program& program)
    :mesh_(std::move(mesh)),program_(&program) {}
vng::resources::Result<void> render(vng::opengl::Frame&,const vng::render::RenderView&,
    std::span<const mesh_shading::MeshDraw>);`,
          ["GpuMesh and two reusable instance-buffer/CPU-record batches."], ["Runtime-owned shared Program; Frame/view/tickets during render."], ["walk-renderer", "walk-instance-buffer", "walk-runtime"], true),
        type("mailbox", "editor_example::PreviewMailbox / PresentedView", "Keep completed pixels paired with the state and camera that produced them.",
          "Image data and revision acknowledgments can arrive separately. offer retains at most the newest completed image for the active worker generation; take waits until the authored revision is known and rejects stale results. PresentedView records the actual displayed camera/frame metadata for overlays and picking. It must not pretend a pending camera request has already been rendered.",
          "examples/editor/preview_mailbox.hpp", `// After a worker frame arrives:
mailbox.offer(std::move(completed_frame), generation);
auto accepted = mailbox.take(generation, minimum_revision,
                             displayed_revision, authored_revision);
// Display only the accepted frame, with its matching metadata.`,
          ["At most one waiting completed image; presented-image metadata is retained by the UI."], ["No live worker State object; uses numeric generations/revisions and captured metadata."], ["walk-worker-endpoint", "walk-preview-session", "code-example-presentation"])
      ])
    ]
  });
})();
