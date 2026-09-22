/* Source-verified component map. Classic script: works without a server.
 * Dependencies describe direct CMake links, not runtime ownership or every include.
 * See maintenance.md and tests/content.test.cjs before updating this inventory. */
(() => {
  "use strict";
  const source = file => ({ label: file, href: `../../${file}` });
  const component = (target, title, summary, description, files, dependencies = [], options = {}) => ({
    id: `code-${target.replace(/^vng_/, "").replaceAll("_", "-")}`,
    title, kind: options.kind || "Library", summary, description: [description],
    links: files.map(source),
    component: {
      target, type: options.type || "Static library",
      dependencies: dependencies.map(target => ({ target, visibility: options.visibility || "PUBLIC" })),
      external: options.external || [],
      buildAfter: options.buildAfter || []
    },
    ...(options.extra || {})
  });
  const external = (...names) => names.map(name => ({ name, visibility: "PRIVATE" }));
  const neutral = [
    component("vng_core", "Core values", "Fixed-width numbers, vectors, matrices, and shared low-level vocabulary.",
      "Start with core/types.hpp to understand values passed between CPU code, shader types and backends. This is a header-only target, not an engine singleton.",
      ["include/vng/core/types.hpp", "include/vng/core/monotonic_clock.hpp"], [], { type: "Header-only (INTERFACE)" }),
    component("vng_gfx", "Geometry & camera data", "Semantic records, codecs, vertex streams, meshes, cameras and neutral buffer/image requests.",
      "gfx contains CPU data and device-dispatched entry points. It does not own an OpenGL context. Record byte layout is calculated explicitly; uploaded resources have separate lifetimes.",
      ["include/vng/gfx/record.hpp", "include/vng/gfx/mesh.hpp", "include/vng/gfx/camera.hpp", "include/vng/gfx/buffer.hpp", "tests/gfx"], ["vng_core"],
      { type: "Header-only (INTERFACE)", visibility: "INTERFACE", extra: { related: ["semantics", "meshes", "camera"] } }),
    component("vng_spatial", "Geometry queries", "An immutable triangle BVH accelerates picking within a mesh.",
      "TriangleBvh owns local-space geometry and an acceleration tree. EditableMesh retains a reusable picking index. The editor still walks candidate instances; this is not a scene-wide spatial database.",
      ["include/vng/spatial/triangle_bvh.hpp", "src/spatial/triangle_bvh.cpp", "tests/editor/picking_acceleration_tests.cpp"], ["vng_core"]),
    component("vng_timeline", "Time & value tracks", "Timestamped values, interpolation and timeline sampling without a renderer.",
      "The engine timeline knows about tracks and sampling. Editor-specific rules such as initial pose, keyframe insertion, selection and undo live in examples/editor, not in this library.",
      ["include/vng/timeline/timeline.hpp", "src/timeline/timeline.cpp", "tests/timeline/timeline_tests.cpp"], ["vng_core"]),
    component("vng_resources", "Resource provider protocol", "Keep reconstruction recipes distinct from the resources they produce.",
      "A provider offers provide(...). Retained provider wrappers can type-erase and share that recipe; concrete owners decide how to reload and commit replacements. This is not a global resource manager.",
      ["include/vng/resources/provider.hpp", "include/vng/resources/shared.hpp", "tests/resources/provider_tests.cpp"], ["vng_core"],
      { type: "Header-only (INTERFACE)", visibility: "INTERFACE", extra: { related: ["resources"] } }),
    component("vng_render", "Rendering contracts", "Renderer tickets, views, frames and typed state/program facades.",
      "Renderer<Ticket, Backend> supplies compile-time identity, not draw policy. Factories such as compile_program dispatch through the device type. OpenGL integration headers live in render_opengl/ and the other *_opengl directories; nothing under render/ includes a backend.",
      ["include/vng/render/renderer.hpp", "include/vng/render/program.hpp", "include/vng/render/graphics_state.hpp", "tests/render"], ["vng_gfx", "vng_resources"],
      { type: "Header-only (INTERFACE)", visibility: "INTERFACE", extra: { related: ["renderer", "graphics-state", "code-backend-dispatch"] } }),
    component("vng_analysis", "Render evidence data", "Image evidence, provenance and inspection contracts for diagnostic rendering.",
      "Neutral analysis types describe captured results and their relationship to entities, primitives and observations. Producing pixels and enhanced shaders belongs to the rendering integration, not this target.",
      ["include/vng/analysis/manifest.hpp", "include/vng/analysis/analysis.hpp", "tests/analysis"], ["vng_gfx"],
      { type: "Header-only (INTERFACE)", visibility: "INTERFACE", extra: { related: ["diagnostics"] } }),
    component("vng_content", "Files & typed extraction", "Parse mesh/documents and decode images before creating GPU resources.",
      "The generic document and .vmesh APIs expose typed reads and semantic field mapping. Scene application meaning is assigned by editor project code. PNG decoding is a private library dependency.",
      ["include/vng/content/document.hpp", "include/vng/content/vmesh_schema.hpp", "src/content/document_parser.cpp", "tests/content"], ["vng_gfx"],
      { external: external("PNG::PNG"), extra: { related: ["files"] } }),
    component("vng_shader", "Shader DSL & typed IR", "Execute shader-building C++ once, record operations and link stage contracts.",
      "Expr<T> is a handle into a FunctionBuilder, not a CPU value or per-expression virtual object. Stage factories validate the recorded function; shader::link combines compatible interfaces. Dynamic draw arguments have a typed CPU-side packing contract.",
      ["include/vng/shader/dsl/expr.hpp", "include/vng/shader/stage.hpp", "include/vng/shader/ir.hpp", "include/vng/shader/arguments.hpp", "src/shader", "tests/shader"], ["vng_gfx"],
      { extra: { related: ["shader-dsl", "shader-arguments"] } }),
    component("vng_providers", "Reusable creation recipes", "Adapters for mesh, program, image and target resource sources; font and skin providers live beside their features.",
      "These recipes connect lower-level provider machinery to content and rendering contracts. A device-specific provide call may create a GPU resource through a facade; the provider is not the long-lived renderer owner.",
      ["include/vng/providers/providers.hpp", "include/vng/providers/program.hpp", "include/vng/providers/mesh.hpp"],
      ["vng_resources", "vng_render", "vng_shader", "vng_content"], { type: "Header-only (INTERFACE)", visibility: "INTERFACE" }),
    component("vng_rig", "Armatures, poses & skin bindings", "CPU rigging data keeps shape, bone hierarchy, weights and animation pose separate.",
      "Armature stores rest structure; Pose stores changing local transforms. SkinBinding connects a mesh snapshot with an armature and weights. The dependency direction is rig_opengl → rig, never rig → rig_opengl.",
      ["include/vng/rig/armature.hpp", "include/vng/rig/skin_binding.hpp", "include/vng/rig/skinned_mesh_renderer.hpp", "src/rig", "tests/rig"], ["vng_gfx", "vng_render"]),
    component("vng_text", "Font shaping & glyph data", "Fonts, shaped text and glyph bitmaps, without issuing OpenGL draws.",
      "Font uses FreeType and HarfBuzz internally. Its metrics and shaped runs are consumed by UI and text-rendering integrations. GPU atlas ownership belongs to the concrete text renderer.",
      ["include/vng/text/font.hpp", "include/vng/text/text_renderer.hpp", "src/text", "tests/text"], ["vng_core"], { external: external("Freetype::Freetype", "vng_harfbuzz_dependency") }),
    component("vng_ui", "Widgets & interaction", "Screen owns retained widgets; updates produce events and drawing data.",
      "Buttons, text fields, lists, scrollbars and themes are modeled here. Screen handles layout/input and prepares a DrawList. Rendering the DrawList is a separate backend concern; ICU supports text interaction.",
      ["include/vng/ui/ui.hpp", "include/vng/ui/ui_renderer.hpp", "src/ui/ui.cpp", "tests/ui/ui_tests.cpp"], ["vng_text", "vng_gfx", "vng_render"], { external: external("ICU::uc", "ICU::i18n") }),
    component("vng_editor", "Reusable editing primitives", "Editable mesh topology, selection and inspector descriptions—not the whole editor app.",
      "These public primitives can be used without the editor UI process. Application document semantics and undo authority belong to EditingSession in examples/editor. The similarly named targets are intentionally distinguished here.",
      ["include/vng/editor/mesh.hpp", "include/vng/editor/inspector.hpp", "include/vng/editor/selection.hpp", "src/editor/mesh_topology.cpp"], ["vng_content", "vng_spatial"]),
    component("vng_editor_preview", "Linux preview transport", "Build/restart a worker, exchange control messages and receive completed RGBA frames.",
      "PreviewSession owns its child processes and transport; WorkerEndpoint is the worker side. Polling does not wait for compilation or a frame. This is trusted project-code fault isolation, not a security sandbox or an editor document model.",
      ["include/vng/editor/preview.hpp", "src/editor/preview.cpp", "tests/editor/preview_tests.cpp"], ["vng_editor"], { external: external("Threads::Threads") })
  ];
  const backends = [
    component("vng_glsl", "GLSL emission", "Lower neutral shader IR to deterministic GLSL, with no live graphics context.",
      "The emitter produces stage sources, interface metadata and source-to-node mapping. Enhanced emission adds diagnostic outputs from the same shader program; driver compilation happens later.",
      ["include/vng/glsl/emitter.hpp", "src/glsl/emitter.cpp", "tests/glsl"], ["vng_shader"], { kind: "Backend" }),
    component("vng_opengl", "OpenGL device & resources", "Own actual buffers/programs/images and execute frame-scoped draw commands.",
      "Device validates context/thread ownership. Move-only resource wrappers and Frames govern GPU lifetimes; Commands borrows the frame's active state. GLAD loads functions. This target does not depend on GLFW.",
      ["include/vng/opengl/device.hpp", "include/vng/opengl/frame.hpp", "include/vng/opengl/commands.hpp", "src/opengl/compile_program.cpp", "tests/opengl"],
      ["vng_gfx", "vng_render", "vng_glsl", "vng_resources"], { kind: "Backend", external: external("vng_glad_dependency") }),
    component("vng_window_glfw", "Native window & input", "GLFW implements window creation, input collection and event polling.",
      "Window-neutral declarations live under window/ and input/. GlfwWindow is the concrete window implementation; pairing it with an OpenGL context is another target. Native window lifetime is distinct from a GPU resource lifetime.",
      ["include/vng/window/window.hpp", "include/vng/window/glfw.hpp", "src/window/glfw.cpp", "tests/window"], ["vng_core"], { kind: "Backend", external: external("vng_glfw_dependency") }),
    component("vng_glfw_opengl", "GLFW ↔ OpenGL bridge", "The explicit integration point between native windows and GL context creation/presentation.",
      "The bridge knows both backends so neither needs to import the other. Example GlfwOpenGLSession owns this window plus its Device, in an order that keeps the context alive during device destruction.",
      ["include/vng/glfw_opengl/glfw_opengl.hpp", "src/glfw_opengl/glfw_opengl.cpp", "examples/support/glfw_opengl_session.hpp"],
      ["vng_opengl", "vng_window_glfw"], { kind: "Integration", external: external("vng_glfw_dependency") }),
    component("vng_render_opengl", "Program runtime & analysis integration", "Realize normal and diagnostic shader paths and collect render evidence.",
      "OpenGLProgramRuntime groups compiled shader products and capture machinery used by concrete renderers. It is an optional integration, not a mandatory superclass or an owner of all graphics settings.",
      ["include/vng/render_opengl/program_runtime.hpp", "src/render_opengl/program_runtime.cpp", "tests/render_opengl/program_runtime_tests.cpp"],
      ["vng_render", "vng_analysis", "vng_glsl", "vng_opengl"], { kind: "Integration" }),
    component("vng_resources_opengl", "Resource-backed mesh renderer", "Build and reload the resources needed by the reusable ticket-driven MeshRenderer.",
      "This renderer owns its mesh/program/texture realization and retained providers. Per-submission state remains in tickets. The editor's BlueprintMeshRenderer is a different application policy, not this class under another name.",
      ["include/vng/resources_opengl/mesh_renderer.hpp", "include/vng/opengl/resources.hpp", "src/resources_opengl/mesh_renderer.cpp", "tests/opengl/resource_owner_tests.cpp"],
      ["vng_opengl", "vng_providers"], { kind: "Integration", external: external("vng_glad_dependency") }),
    component("vng_bloom_opengl", "Bloom renderer", "Extract and blur bright light, then composite it into a displayable image.",
      "This implementation owns its postprocessing resources and exposes the bloom rendering facade. Sun-specific surface patterns and artistic policy remain in the sun example helpers.",
      ["include/vng/render/bloom.hpp", "include/vng/bloom_opengl/bloom.hpp", "src/bloom_opengl/bloom.cpp", "tests/opengl/bloom_tests.cpp"],
      ["vng_opengl", "vng_resources"], { kind: "Integration", external: external("vng_glad_dependency") }),
    component("vng_rig_opengl", "Skinned-mesh renderer", "Use CPU skin bindings and changing bone palettes to deform mesh vertices on the GPU.",
      "The concrete renderer owns its GPU realization and depends on the neutral rig library. A caller supplies the pose/view through its rendering API; the rig library never calls this renderer.",
      ["include/vng/rig_opengl/skinned_mesh_renderer.hpp", "src/rig_opengl/skinned_mesh_renderer.cpp", "tests/opengl/skinned_renderer_tests.cpp"],
      ["vng_rig", "vng_render_opengl", "vng_resources"], { kind: "Integration", external: external("vng_glad_dependency") }),
    component("vng_text_opengl", "Text renderer", "Turn text tickets into GPU glyph batches and maintain atlas resources.",
      "The renderer combines neutral font shaping with OpenGL resources and the provider protocol. Persistent glyph caching belongs here, not in each button or label.",
      ["include/vng/text/text_renderer.hpp", "include/vng/text_opengl/text_renderer.hpp", "src/text_opengl/text_renderer.cpp", "tests/opengl/text_renderer_tests.cpp"],
      ["vng_opengl", "vng_text", "vng_resources"], { kind: "Integration", external: external("vng_glad_dependency") }),
    component("vng_ui_opengl", "UI renderer", "Draw UI shapes and text from a Screen's drawing data.",
      "This implementation consumes the neutral UI representation and reuses TextRenderer. It does not own editor selection, scene state or undo history.",
      ["include/vng/ui/ui_renderer.hpp", "include/vng/ui_opengl/ui_renderer.hpp", "src/ui_opengl/ui_renderer.cpp", "tests/opengl/ui_renderer_tests.cpp"],
      ["vng_ui", "vng_text_opengl"], { kind: "Integration", external: external("vng_glad_dependency") })
  ];
  const applications = [
    component("vng_earth_assets", "Earth asset authoring", "Generate the stylized Earth mesh and edit its named cloud formations on the CPU.",
      "make_mesh builds the Earth blueprint offline as an ordinary .vmesh document. Cloud settings, formation identities and move/turn/add/remove edits live in that document, so the editor can present them as ordinary blueprint controls. This is procedural content policy, not a renderer: it depends on vng_content only, never on OpenGL or the editor.",
      ["examples/support/earth_assets.hpp", "examples/support/earth_clouds.hpp", "examples/support/earth_edit.cpp", "tests/examples/earth_assets_tests.cpp"], ["vng_content"], { kind: "App library" }),
    component("vng_editor_project", "Editor project model", "Authored scenes, private viewport state, editing transactions, history and persistence.",
      "EditingSession is the authoring authority. The target also contains project codecs, narrow document patches, navigation math and preview-update coalescing. These are editor application policies, despite being built as a static library. It links vng_earth_assets so the Earth blueprint's cloud formations can be described as ordinary inspector controls.",
      ["examples/editor/editing_session.hpp", "examples/editor/project.hpp", "examples/editor/document_changes.hpp", "examples/editor/preview_updates.hpp", "tests/editor/editing_session_tests.cpp"],
      ["vng_editor", "vng_timeline", "vng_earth_assets"], { kind: "App library", extra: { related: ["code-session-owner", "code-edit-delivery"] } }),
    component("vng_example_support", "Example window-loop helpers", "Shared startup, options, diagnostics and native-loop repetition for demos.",
      "Small demo entry points delegate setup here so their rendering code remains readable. GlfwOpenGLSession is an example-owned backend pair, not a compulsory engine application object.",
      ["examples/support/glfw_opengl_session.hpp", "examples/support/window_loop.hpp", "examples/support/diagnostics.hpp"], ["vng_glfw_opengl"], { kind: "App library" }),
    component("vng_example_presentation", "Example image presentation", "Display completed images and write captures using explicit OpenGL resources.",
      "DisplaySurface is an application helper used by examples and the editor runtime/UI. It is not the generic preview transport and does not own the authored scene.",
      ["examples/support/presentation.hpp", "examples/support/presentation.cpp"], ["vng_opengl"], { kind: "App library", external: external("vng_glad_dependency", "PNG::PNG") }),
    component("vng_sun_support", "Sun effect implementation", "Procedural surface, displacement, strands, softening and diagnostics for the sun demo.",
      "Effect-specific C++ shaders and resources live in these example helpers. Editing artistic behavior starts in sun_shaders.cpp or sun_renderer.cpp, not in the generic shader IR or bloom library.",
      ["examples/support/sun_renderer.hpp", "examples/support/sun_renderer.cpp", "examples/support/sun_shaders.cpp", "examples/support/sun_softening.cpp"], ["vng_render_opengl"],
      { kind: "App library", external: external("vng_glad_dependency") }),
    component("vng_editor_runtime", "Scene GPU realization", "Per-blueprint mesh renderers, effect resources and off-screen frame production.",
      "Runtime owns the worker's scene rendering resources. It borrows State and a Device for operations; it does not become the authoring authority. Scene demos reuse this runtime without the editor UI.",
      ["examples/editor/runtime.hpp", "examples/editor/runtime.cpp", "examples/editor/blueprint_mesh_renderer.hpp", "tests/editor/runtime_tests.cpp", "tests/editor/mesh_batch_tests.cpp"],
      ["vng_editor_project", "vng_sun_support", "vng_bloom_opengl", "vng_example_presentation"],
      { kind: "App library", external: external("vng_glad_dependency"), extra: { related: ["code-runtime-owner"] } }),
    component("vng_editor_worker", "Preview worker executable", "Own a GL context, apply document/view requests, sample playback and produce scene frames.",
      "The worker's context-owning loop performs playback sampling, effect callbacks and rendering. There is no separate simulation thread hidden behind it. Independent Play opens a worker-owned window; debug communication can stay enabled or be disabled.",
      ["examples/editor_worker.cpp", "examples/editor/viewport_session.hpp", "tests/editor/worker_tests.cpp"],
      ["vng_editor_runtime", "vng_editor_preview", "vng_example_support"], { kind: "Executable", type: "Executable", visibility: "PRIVATE" }),
    component("vng_editor_ui", "Editor panels, dialogs & gizmo tools", "Retained sidebar panels, file dialogs and viewport tools drawn through vng_ui, without a GL context.",
      "InspectorPanel, TimelinePanel, BlueprintMeshPanel, the file dialogs and the translation/rotation/scale/cage/region tools live here. They read a const State and submit typed edits to EditingSession; the app composes them and the UI tests drive them headlessly. Rendering their DrawList is vng_ui_opengl's job.",
      ["examples/editor/inspector_panel.hpp", "examples/editor/timeline_panel.hpp", "examples/editor/transform_gesture.hpp", "examples/editor/file_dialog.hpp", "tests/editor/inspector_panel_tests.cpp"],
      ["vng_editor_project", "vng_ui"], { kind: "App library", extra: { related: ["code-ui-owner"] } }),
    component("vng_editor_app", "Editor UI application", "Assemble the session, retained controls, tools, worker communication and displayed preview.",
      "app.cpp is the composition point and event loop. Scene, Keyframe values and Instance properties share one right sidebar. Detached editable preview remains UI-process-owned; it is not independent Play and does not create another authoring session.",
      ["examples/editor/app.cpp", "examples/editor/editor_layout.hpp", "examples/editor/viewport_window.hpp", "examples/editor/automation.hpp", "tests/editor/e2e_tests.cpp"],
      ["vng_editor_ui", "vng_editor_preview", "vng_ui_opengl", "vng_example_support", "vng_example_presentation"],
      { kind: "App library", external: external("vng_glad_dependency"), buildAfter: ["vng_editor_worker"], extra: { related: ["code-ui-owner"] } }),
    component("vng_editor_demo", "Editor entry point", "Parse command-line options and enter the editor application.",
      "examples/editor.cpp delegates to editor_example::run in app.cpp. Start here to understand application startup; then choose authoring, tools, transport or rendering rather than reading the entire repository linearly.",
      ["examples/editor.cpp", "examples/editor/app.hpp"], ["vng_editor_app"], { kind: "Executable", type: "Executable", visibility: "PRIVATE" })
  ];
  // One private link to another mapped application target, not an external package.
  applications.find(node => node.component.target === "vng_sun_support").component.dependencies.push({ target: "vng_example_presentation", visibility: "PRIVATE" });

  window.VNG_GUIDE.splice(1, 0, {
    id: "codebase", title: "Codebase", kind: "Source map",
    summary: "Find the real component, its dependencies and the files to change—not just the feature name.",
    description: ["Choose a branch: the repository tour tells you where code lives; the component catalog shows direct build dependencies; the ownership tree explains who keeps runtime state alive. They answer different questions."],
    boundary: "This is a curated source-verified map, not a live C++ include graph. Direct CMake links are checked by documentation tests. Used-by lists cover mapped components, not every demo/test. Conditional targets may be disabled in your build.",
    links: [{ label: "Codebase source tour (Markdown)", href: "../codebase.md" }, source("CMakeLists.txt")],
    children: [
      {
        id: "code-repository", title: "Where files live", kind: "Orientation",
        summary: "Public headers, implementations, applications and tests have different jobs.",
        description: ["include/vng is the public C++ surface. src holds compiled implementations. Some libraries are header-only, so no matching .cpp is needed. Directory names and CMake targets do not always match one-to-one.", "examples contains runnable programs and application-specific policy. examples/support hides setup and effect details; examples/editor contains the editor application. tests includes compile-time failures, CPU tests, OpenGL tests and native editor workflows. docs explains the system; assets are data, not APIs."],
        boundary: "A folder is not an owner. A CMake dependency is not a member field. Follow the explicit ownership branch for lifetimes.",
        links: [source("include/vng"), source("src"), source("examples/README.md"), source("tests"), source("CMakeLists.txt")]
      },
      {
        id: "code-components", title: "Components & direct dependencies", kind: "Build map",
        summary: "Open a component to see what it needs and which mapped components use it.",
        description: ["Each card names a real CMake target and links to its API, implementation and relevant tests. Depends on means a direct target_link_libraries entry; Used by is derived from the same entries, so the arrows cannot disagree.", "PUBLIC exposes a linked dependency to consumers; INTERFACE exposes it from a header-only target; PRIVATE records an implementation link. These labels are CMake usage requirements, not promises that static-library consumers need no transitive linker inputs. Build-after is ordering only, not a linked dependency."],
        links: [source("CMakeLists.txt"), source("cmake/Dependencies.cmake")],
        children: [
          { id: "code-neutral", title: "Backend-neutral libraries", kind: "Build map", summary: "Values, contracts and CPU work do not require an OpenGL context.", description: ["Neutral does not mean dependency-free: text uses font libraries, content decodes PNG, and Linux preview transport manages processes. It means these components do not make OpenGL their core data model."], children: neutral },
          { id: "code-backends", title: "Backends & integrations", kind: "Build map", summary: "Concrete implementations depend on contracts; bridges explicitly know both sides.", description: ["GLSL emission, OpenGL execution and GLFW windowing are distinct boundaries. Higher-level rendering integrations choose the pieces they need rather than making the neutral Renderer base depend on every feature."], children: backends },
          { id: "code-applications", title: "Editor & supporting application targets", kind: "Build map", summary: "The UI process and worker link different pieces of the same engine.", description: ["These targets package application code for reuse and testing. They are not all public engine libraries. The editor application has a build-after requirement for its worker executable but does not link that executable into the UI process."], children: applications }
        ]
      },
      {
        id: "code-ownership", title: "Runtime ownership: who keeps what alive?", kind: "Ownership",
        summary: "The editor application has one authoring owner and a separate preview worker.",
        description: ["These branches describe actual runtime containment. Helper classes shown as children are owned objects or named groups of owned local objects, not new classes invented for this diagram. Links identify borrowed collaborators."],
        children: [
          {
            id: "code-ui-owner", title: "UI process · editor_example::run", kind: "Owner",
            summary: "Own the editing session, controls/tools and preview communication—not the worker's GPU scene.",
            description: ["The application composes these objects as local owners in app.cpp. Tools submit edits to the session; preview delivery consumes edit notices; controls render through UI resources. A detached viewport uses a second UI-owned window/context while sharing the same session."],
            ownership: { owns: ["EditingSession", "UI Screens, panels and selection sets", "ViewportInteraction", "PreviewSession + PreviewUpdates + PreviewMailbox", "Displayed image and optional DetachedViewportWindow"], borrows: ["An optional Automation supplied in Options, for native tests"] },
            links: [source("examples/editor/app.cpp"), source("examples/editor/viewport_window.hpp")],
            children: [
              {
                id: "code-session-owner", title: "EditingSession", kind: "Owner",
                summary: "One authority for authoring, transaction commit/cancel, undo/redo and file identity.",
                description: ["State holds Document and ViewportState side by side. state() exposes const read access; viewport() allows private inspection changes. Typed editing methods publish EditNotice values, so this owner needs neither widgets nor IPC nor a GPU.", "Document owns blueprints, applied mesh geometry, separate mesh drafts, scene instances, the authored camera and timeline. Instance transforms and region boundaries belong to instances. ViewportState owns private camera, active inspection/playhead state and its own sequence; full multi-selection sets remain in UI tools/panels."],
                ownership: { owns: ["State { Document, ViewportState }", "Active gesture checkpoint and undo/redo deques", "EditClipboard", "SceneFile and saved-content identity"], borrows: [] },
                api: "// With an existing session and selected instance ID:\nauto started = editing.begin_move(instance_id);\nif (!started) return std::unexpected(started.error());\nauto moved = editing.move(new_position);\nif (!moved) {\n    auto cancelled = editing.cancel();\n    if (!cancelled) return std::unexpected(cancelled.error());\n    return std::unexpected(moved.error());\n}\nreturn editing.commit(); // One undo transaction.",
                links: [source("examples/editor/editing_session.hpp"), source("examples/editor/project.hpp"), source("examples/editor/session_operations.cpp"), source("tests/editor/editing_session_tests.cpp")],
                related: ["editing-session", "mesh-drafts", "code-edit-delivery"]
              },
              {
                id: "code-tools-owner", title: "ViewportInteraction & panels", kind: "Owner",
                summary: "Own interaction tools; arbitrate one active gesture without giving tools global control.",
                description: ["ViewportInteraction owns navigation, walk, region boundary, transform, pivot, mesh and box-selection tools. It explicitly borrows EditingSession. Panels keep widget handles and own local drafts; Screen owns widget storage. The application supplies model updates and eligibility.", "G/R/S component transforms and region boundary editing share a component-transform helper. Sidebar tabs switch existing panels rather than reconstructing the scene or resetting their scroll state."],
                ownership: { owns: ["Interaction tools and capture arbitration", "Tool-local point selections and gesture data"], borrows: ["EditingSession&", "UI container handles backed by the Screen"] },
                links: [source("examples/editor/viewport_interaction.hpp"), source("examples/editor/component_transform.hpp"), source("examples/editor/timeline_panel.hpp"), source("examples/editor/editor_layout.hpp")]
              },
              {
                id: "code-preview-owner", title: "Preview delivery & presentation", kind: "Owner group",
                summary: "Deliver authored changes, send latest viewing intent and display a coherent completed frame.",
                description: ["PreviewSession owns build/worker processes and transport. PreviewUpdates coalesces authored changes while waiting for acknowledgments. PreviewMailbox keeps a completed image until the corresponding authored revision is known. These are sibling local objects in the application, not children of EditingSession.", "Presented frame metadata describes the pixels actually shown, including camera pose, time, extent and revision. Picking/overlays must not pretend an older image already depicts the newest requested camera."],
                ownership: { owns: ["Process/transport handles in PreviewSession", "Per-worker delivery state in PreviewUpdates", "Pending completed image in PreviewMailbox"], borrows: ["State when preparing a document update", "Frame metadata when projecting overlays"] },
                links: [source("include/vng/editor/preview.hpp"), source("examples/editor/preview_updates.hpp"), source("examples/editor/preview_mailbox.hpp"), source("examples/editor/presented_view.hpp")],
                related: ["code-editor-preview", "code-edit-delivery"]
              }
            ]
          },
          {
            id: "code-worker-owner", title: "Worker process", kind: "Owner",
            summary: "Keep worker state, context and runtime GPU objects together on the context-owning thread.",
            description: ["editor_worker.cpp assembles a WorkerEndpoint, a worker-side State, a window/device pair and Runtime. Its loop handles requests, samples playback, renders, then publishes a completed image or presents an independent window. It does not share mutable Document objects or GPU handles with the UI process."],
            links: [source("examples/editor_worker.cpp")], related: ["code-editor-worker"],
            children: [
              {
                id: "code-runtime-owner", title: "Runtime → per-blueprint renderers", kind: "Owner",
                summary: "One GPU mesh realization per blueprint; one renderer submission per visible mesh blueprint.",
                description: ["Runtime::Impl owns a stable shared mesh program, a map of blueprint MeshResources, diagnostic program, sun renderer, HDR targets, softening, bloom and presentation resources. Each BlueprintMeshRenderer owns its GPU mesh and reusable solid/wireframe instance buffers.", "BlueprintMeshRenderer explicitly borrows the runtime-owned Program, whose lifetime encloses all blueprint renderers. Runtime borrows State, camera/view inputs and Device for each call. Compatible instances become one instanced draw; solid and wireframe split into at most two mesh batches per blueprint."],
                ownership: { owns: ["Shared mesh program, declared before blueprint renderers", "Per-blueprint CPU provenance + GPU mesh renderer", "Effects, postprocess and frame targets"], borrows: ["Device& and const State& for operations", "Each child mesh renderer borrows the shared Program"] },
                links: [source("examples/editor/runtime.hpp"), source("examples/editor/runtime.cpp"), source("examples/editor/blueprint_mesh_renderer.hpp"), source("tests/editor/mesh_batch_tests.cpp")],
                related: ["batching", "code-editor-runtime"]
              }
            ]
          }
        ]
      },
      {
        id: "code-follow", title: "Follow one change through the source", kind: "Walkthrough",
        summary: "Concrete routes from an API call or editor gesture to its implementation and tests.",
        description: ["Pick the route nearest your task. Each stops at a meaningful boundary rather than introducing a new manager or service."],
        children: [
          {
            id: "code-backend-dispatch", title: "A neutral call becomes an OpenGL operation", kind: "Walkthrough",
            summary: "render::compile_program(device, shader) selects the implementation from the device's type.",
            steps: ["The application builds shader stages and links their typed interfaces into a shader program.", "render/program.hpp forwards compile_program to compile_graphics_program through C++ argument-dependent lookup (ADL). The opengl::Device argument selects the OpenGL overload.", "src/opengl/compile_program.cpp emits GLSL and creates the driver program. The result is a backend resource, not a neutral program secretly containing all backends.", "A frame lends commands. run(program, values...) selects that program and uploads typed arguments; view(view) supplies camera data; draw(mesh) issues geometry. Graphics settings remain explicit."],
            api: "auto program = vng::render::compile_program(device, shader);\nif (!program) return std::unexpected(program.error());\n\nauto commands = frame.render_context();\nif (auto r = commands.run(*program, model, brightness); !r)\n    return r;\nif (auto r = commands.view(view); !r) return r;\nreturn commands.draw(gpu_mesh);",
            boundary: "The excerpt assumes a shader with model/brightness arguments and an existing frame. Complete error/result types and context lifetime setup are in file_mesh_direct.cpp.",
            links: [source("include/vng/render/program.hpp"), source("src/opengl/compile_program.cpp"), source("include/vng/opengl/commands.hpp"), source("examples/file_mesh_direct.cpp"), source("tests/render/program_tests.cpp")],
            related: ["code-render", "code-opengl", "code-glsl"]
          },
          {
            id: "code-edit-delivery", title: "An edit reaches the preview", kind: "Walkthrough",
            summary: "Authored values and private viewing requests travel on different update lanes.",
            steps: ["A panel/tool submits a typed EditingSession operation. A gesture stages changes and commit creates one undo record.", "take_changes() yields an EditNotice with DocumentChanges. PreviewUpdates combines affected property/vertex IDs and sends an ordered DocumentPatch or necessary structural snapshot.", "The worker validates and applies the update, acknowledges its authored revision and invalidates affected GPU resources. A position-only update can patch explicit vertex positions instead of reloading the mesh.", "Private camera navigation/selection uses an absolute ViewportRequest and send_latest. New viewing intent can replace old intent; authored deltas must never use this lane.", "The worker renders from current state. PreviewMailbox and frame metadata keep displayed pixels consistent with known revisions. The UI draws local overlays and controls."],
            boundary: "A camera-only request does not serialize the document or create an authored revision. Structural edits, reconnects and recovery may require full snapshots; not every update can be a tiny patch.",
            links: [source("examples/editor/document_changes.hpp"), source("examples/editor/document_patch.cpp"), source("examples/editor/preview_updates.cpp"), source("examples/editor/viewport_session.hpp"), source("examples/editor_worker.cpp"), source("tests/editor/update_tests.cpp")]
          },
          {
            id: "code-where-to-change", title: "Where should I change a feature?", kind: "Source tour",
            summary: "Start in the narrowest owner; follow tests before expanding an abstraction.",
            description: ["New draw policy: start with FileMeshRenderer and its ticket; keep shader creation/resources inside the concrete renderer. New file field mapping: start with vmesh_schema.hpp and the direct mesh example. New sun look: start with sun_shaders.cpp.", "New editor authoring operation: EditingSession + DocumentChanges/DocumentPatch + focused tests. New gizmo/selection behavior: ViewportInteraction and the specific tool; geometry acceleration is in spatial/EditableMesh. New UI placement: app.cpp and editor_layout.hpp, not the worker.", "Playback rendering or batching: runtime.cpp and BlueprintMeshRenderer. Preview delivery latency: PreviewUpdates, the viewport request lane, transport and interaction timing. Distinguish CPU, worker rendering and GPU-readback time before optimizing."],
            links: [source("examples/file_mesh_renderer.hpp"), source("examples/file_mesh_renderer.cpp"), source("examples/editor/blueprint_gizmos.hpp"), source("examples/editor/automation.hpp"), source("include/vng/editor/interaction_timing.hpp"), source("tests/editor/e2e_tests.cpp"), source("tests/compile_fail")]
          }
        ]
      }
    ]
  });
})();
