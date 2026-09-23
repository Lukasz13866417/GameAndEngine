/* Content only. Paths are relative to index.html; see README.md for the schema. */
(() => {
  "use strict";
  const guide = (name, file) => ({ label: `${name} · detailed guide (Markdown)`, href: `../${file}.md` });
  const source = (file) => ({ label: file, href: `../../${file}` });
  window.VNG_GUIDE = [
    {
      id: "start", title: "Start here", kind: "Orientation",
      summary: "What this project is, and the few words you need to understand it.",
      description: ["Vibe Engine is a C++23 engine and an editor being developed together. The engine supplies reusable building blocks; the editor is an application that uses those blocks to author scenes."],
      boundary: "This is a map of the current implementation, not a proposed future architecture. OpenGL 4.6 on Linux is the implemented graphics backend.",
      children: [
        {
          id: "big-picture", title: "The big picture", kind: "Concept",
          summary: "Data describes a scene. Renderers turn that data into an image. The editor lets you change it.",
          description: ["Think of a spaceship: its mesh describes its shape, a scene instance gives it a position, and a renderer decides how to draw it. The editor changes the scene data; it does not replace the renderer with a separate visual approximation."],
          boundary: "Reusable public APIs live in include/vng; their implementations live in src. Application policies and the editor live in examples. An example helper is not automatically an engine-wide abstraction.",
          related: ["blueprints", "renderer", "editor"], links: [source("examples/file_mesh_direct.cpp"), source("examples/file_mesh_renderer.cpp")]
        },
        {
          id: "vocabulary", title: "The essential vocabulary", kind: "Concept",
          summary: "Mesh, shader, backend, resource, and viewport—in ordinary language.",
          description: ["A mesh is a shape made from points and faces. A shader is a small program executed on the graphics processor (GPU). A backend is the implementation for a particular graphics API, such as OpenGL.", "A resource is something kept around for reuse, such as a GPU buffer, font, or texture. A viewport is the editor area in which you look at the scene, together with the viewing and inspection state—not the scene itself."],
          related: ["meshes", "shader-dsl", "window-device", "resources", "viewport"]
        },
        {
          id: "philosophy", title: "How to read the architecture", kind: "Concept",
          summary: "Clear owners, explicit collaborators, and responsibilities with ordinary names.",
          description: ["Ownership answers “who keeps this alive?” A dependency answers “what does this piece need to work?” These are different questions: a renderer can borrow a frame without owning it.", "Prefer an understandable ownership tree. When pieces collaborate, pass their dependencies explicitly. The whole include graph does not have to be a literal tree. This guide groups concepts for learning and labels real ownership where it matters."],
          boundary: "Backend-neutral data describes intent. Backend objects do the actual GPU work. Creating a program does not secretly give it control of depth testing or the render target.",
          related: ["graphics-state", "editing-session"], links: [guide("Renderer API", "renderer_api"), guide("Editor boundaries", "editor_boundaries")]
        }
      ]
    },
    {
      id: "engine", title: "Engine building blocks", kind: "Engine library",
      summary: "The reusable pieces an application combines—not one giant object that owns everything.",
      description: ["These branches are responsibility groups, not a single engine object's members. You choose which pieces your application needs and create the backend resources explicitly."],
      children: [
        {
          id: "data", title: "Shapes, values, and files", kind: "Engine library",
          summary: "Describe what exists without needing a graphics context.",
          description: ["CPU-side data can be loaded, inspected, edited, and tested without opening a window. GPU upload is a separate step."],
          children: [
            {
              id: "semantics", title: "Semantics & records", kind: "Engine library",
              summary: "Say what a value means, separately from how its bytes are stored.",
              description: ["A semantic is a C++ tag such as Position or Color. A Record holds fields addressed by those tags. Encoding describes storage: a color can mean four floating-point components while occupying four normalized bytes.", "The same shader can consume different field orders and compatible encodings. You do not manually keep shader attribute numbers in sync with your mesh layout."],
              api: "struct Position : vng::gfx::Semantic<vng::Vec3> {};\nstruct Color : vng::gfx::Semantic<vng::Vec4> {};\nusing Vertex = vng::gfx::Record<\n    Position, vng::gfx::as<Color, vng::gfx::unorm8x4>>;\n\nVertex vertex;\nvertex.set(Position{}, {0, 1, 0});",
              links: [guide("Shader and vertex path", "shader_vertex_pipeline"), source("include/vng/gfx/record.hpp")]
            },
            {
              id: "meshes", title: "Meshes & vertex streams", kind: "Engine library",
              summary: "A mesh owns vertex data and faces. A GPU mesh is a separate uploaded representation.",
              description: ["A VertexStream is a contiguous collection of records. A Mesh combines one or more per-vertex streams with triangle faces and optional authored edges.", "Uploading creates GPU storage. OpenGL infers and caches the vertex-array binding (VAO) from the mesh and shader semantics; callers do not build it by hand."],
              boundary: "Mesh data is geometry, not an object placed in the world. Instance transforms and per-draw values stay separate.",
              related: ["semantics", "blueprints", "batching"],
              links: [guide("Meshes and .vmesh", "mesh_and_vmesh"), source("include/vng/gfx/mesh.hpp"), source("include/vng/opengl/gpu_mesh.hpp")]
            },
            {
              id: "files", title: "File formats & schemas", kind: "Engine library",
              summary: "Readable files are mapped to typed runtime data at the loading boundary.",
              description: ["A .vmesh file contains general information, vertex fields, faces, and optional edges. A schema maps names in the file to C++ semantic tags.", "The generic document API also supports typed property reads without assuming every object has identical fields. The editor's scene loader gives .vscene documents their application-specific meaning."],
              api: "namespace vmesh = vng::content::vmesh;\nauto schema = vmesh::schema<Vertex>();\nschema.map(\"position\", Position{});\nschema.map(\"color/0\", Color{});\nauto mesh = vmesh::load(path, schema);",
              links: [guide("Mesh format", "mesh_and_vmesh"), guide("Structured documents", "documents"), source("examples/file_mesh_direct.cpp")]
            },
            {
              id: "camera", title: "Camera & render view", kind: "Engine library",
              summary: "Where you look from, and the frozen camera data for one image size.",
              description: ["The camera stores a position, orientation, and projection settings. Perspective makes distant things appear smaller; orthographic projection does not.", "A RenderView is an immutable snapshot for a particular image extent. Passing the same view to drawing and diagnostics keeps their camera and image proportions consistent."],
              boundary: "The editor's private viewing camera and the authored scene camera are separate uses of camera data.",
              related: ["viewport", "frame"], links: [guide("Camera", "camera"), source("include/vng/gfx/camera.hpp")]
            }
          ]
        },
        {
          id: "drawing", title: "Turning data into pixels", kind: "Engine library",
          summary: "Shaders describe calculations. Renderers decide what to draw and how.",
          description: ["Drawing has several distinct responsibilities. Keeping them separate makes it possible to inspect the shader, change draw settings, or use an off-screen image without redesigning everything."],
          children: [
            {
              id: "shader-dsl", title: "Shader DSL → IR → GPU program", kind: "Engine library",
              summary: "Write shader calculations in C++; the engine records and compiles them.",
              description: ["DSL means domain-specific language: here, C++ expressions such as Float and Float3 describe GPU operations instead of calculating ordinary CPU values.", "IR means intermediate representation: the engine's typed description of those operations. The shader lambda runs once to build it. The GLSL emitter turns it into OpenGL shader source, which the driver compiles into a Program."],
              boundary: "IR is backend-neutral; GLSL and the compiled OpenGL Program are not. Shader lambdas are not replayed on every frame.",
              related: ["shader-arguments", "graphics-state", "diagnostics"],
              links: [guide("Shader path", "shader_vertex_pipeline"), source("include/vng/shader/ir.hpp"), source("src/opengl/compile_program.cpp")]
            },
            {
              id: "shader-arguments", title: "Values that change each draw", kind: "Engine library",
              summary: "A shader receives symbolic arguments; your application supplies ordinary values.",
              description: ["Declare lambda arguments with DSL types such as Float or Float4x4. Supply the matching CPU float or matrix when calling run(). Changing that CPU value affects the next submission, without recompiling the shader.", "Capturing a CPU variable while constructing a shader is not a live connection to that variable. Use explicit shader arguments for changing values."],
              api: "auto context = frame.render_context();\ncontext.run(program, model, brightness);\ncontext.view(view);\ncontext.draw(gpu_mesh);",
              boundary: "run() selects the program and supplies arguments. draw() issues the actual draw. This excerpt assumes a program with model and brightness arguments.",
              links: [guide("Typed shader arguments", "typed_shader_arguments"), source("examples/file_mesh_direct.cpp")]
            },
            {
              id: "renderer", title: "Renderer & tickets", kind: "Engine library",
              summary: "A renderer owns drawing policy. A ticket says what this submission needs.",
              description: ["A ticket is a small value containing per-draw information, for example a model transform, tint, and emission. The concrete renderer keeps persistent resources and decides when to change state, batch objects, or invoke other renderers.", "Renderer<Ticket, Backend> is a tiny, non-virtual base identifying ticket and backend types. opengl::Renderer<Ticket> is its concrete alias. There is no implicit portable backend, hidden router or mandatory single-shader technique."],
              api: "// The renderer owns its shaders and persistent resources.\nrenderer.render(frame, view, tickets);",
              related: ["batching", "resources", "graphics-state"], links: [guide("Renderer API", "renderer_api"), source("include/vng/render/renderer.hpp"), source("examples/file_mesh_renderer.hpp")]
            },
            {
              id: "frame", title: "Frame & drawing context", kind: "Backend",
              summary: "Choose the image to draw into, then borrow its command context.",
              description: ["A frame defines the target image, viewport size, output encoding, optional clears, and the lifetime of drawing commands. Its context selects programs, sets state, and draws geometry.", "All command handles borrowed from that frame share its live program and graphics state. Ending the frame invalidates them. Presenting the window is a separate operation."],
              boundary: "A child renderer does not receive fresh default settings. Each renderer establishes what it needs; a parent reselects its own settings before continuing.",
              related: ["graphics-state", "window-device"], links: [guide("Frames and commands", "renderer_api"), source("include/vng/opengl/frame.hpp")]
            },
            {
              id: "graphics-state", title: "Live graphics settings", kind: "Engine library",
              summary: "Depth, culling, and blending are choices for a draw—not properties of a shader.",
              description: ["Depth testing decides whether a surface is hidden behind one already drawn. Culling skips selected triangle orientations. Blending decides how new color combines with the existing image.", "Use the backend-neutral GraphicsState facade with typed settings. The concrete backend implements them; compile-time checks reject unsupported setting types. Backend extensions remain explicit."],
              api: "auto context = frame.render_context();\nauto graphics = context.graphics_state();\ngraphics.set(vng::render::DepthTest{true});\ngraphics.set(vng::render::CullMode::back);\ncontext.run(program);\ncontext.draw(gpu_mesh);",
              boundary: "Programs own shader code and interface metadata. The frame owns output encoding. Neither substitutes for the other.",
              links: [guide("Live graphics state", "graphics_state"), guide("Programs", "graphics_program"), source("include/vng/render/graphics_state.hpp")]
            },
            {
              id: "batching", title: "Many instances, fewer draw calls", kind: "Backend",
              summary: "Keep one shape on the GPU and supply a record for each copy.",
              description: ["GPU instancing draws many copies of the same geometry in one draw call. Each copy can have its own transform and appearance values.", "The reusable MeshRenderer groups adjacent tickets with matching depth and culling choices, preserving order. The editor separately submits mesh tickets per blueprint, splitting solid and wireframe work when necessary."],
              boundary: "One renderer call can contain multiple native draws. This is not a promise that every effect, transparent object, or independently animated skin can be combined into one draw.",
              related: ["blueprints", "renderer"], links: [guide("Resource-backed mesh renderer", "resources"), guide("Editor batching", "editor_boundaries"), source("src/resources_opengl/mesh_renderer.cpp")]
            }
          ]
        },
        {
          id: "resources", title: "Resources & their providers", kind: "Engine library",
          summary: "Separate a reusable creation recipe from the object that owns its result.",
          description: ["A provider knows how to obtain a resource. A builder collects providers and creates a ready owner. The owner keeps the resource and decides which replacements must succeed together.", "Reloading asks a provider for a new value; moving a ready resource transfers ownership. Transactional updates validate replacements before installing them. Ordinary C++ destruction releases owned resources."],
          boundary: "A raw GPU buffer need not retain its original upload bytes. Reconstruction information belongs in the provider. OpenGL resource updates and destruction require the appropriate current context.",
          links: [guide("Resources", "resources"), source("include/vng/resources/resources.hpp")]
        },
        {
          id: "specialized", title: "Text, UI, effects, and rigging", kind: "Engine library",
          summary: "Higher-level features reuse the same drawing and ownership building blocks.",
          description: ["These are collaborators, not subsystems hidden inside every renderer. An application includes the ones it needs."],
          children: [
            {
              id: "ui", title: "UI & text", kind: "Engine library",
              summary: "Persistent controls, readable text, and behavior you can poll with normal if statements.",
              description: ["A Screen holds widgets and a theme. Input updates widget state; your code checks events such as clicked() or changedText(). A backend renderer consumes the UI drawing data and reuses text rendering.", "The text renderer shapes text using fonts and caches glyph images. The UI model does not need to know OpenGL calls."],
              api: "if (button.clicked()) paused = !paused;\nif (auto edited = name.changedText()) {\n    // Copy the text if it must outlive the next widget update.\n    object_name = std::string(*edited);\n}",
              links: [guide("UI", "ui"), guide("Text", "text_rendering"), source("examples/ui.cpp")]
            },
            {
              id: "bloom", title: "Textures, glow & bloom", kind: "Engine library",
              summary: "Render bright light to an image, blur it, and combine it with the scene.",
              description: ["A texture is image data sampled by a shader. An off-screen render target is an image you draw into. HDR means its color values can preserve light brighter than ordinary display white.", "Bloom spreads bright image regions into a glow. The sun example adds its own animated surface and strands, then uses postprocessing. Its effect policy lives in example code, not inside the engine's generic Program."],
              related: ["frame", "resources"], links: [guide("Bloom", "bloom"), guide("Sun example", "sun"), source("examples/support/sun_renderer.cpp")]
            },
            {
              id: "rigging", title: "Rigging: shape, bones, weights", kind: "Engine library",
              summary: "Keep the mesh, skeleton, influence weights, and changing pose distinct.",
              description: ["An Armature holds the bone hierarchy and rest transforms. A Pose holds changing local bone transforms. A SkinBinding connects a mesh snapshot to an armature and its vertex influences—the weights.", "The OpenGL skinned renderer uploads geometry and the bone-matrix palette used to deform it. The CPU rigging library does not depend on the OpenGL implementation."],
              links: [guide("Rigging", "rigging"), source("examples/rigging.cpp")]
            }
          ]
        },
        {
          id: "window-device", title: "Window, graphics device & backend boundary", kind: "Backend",
          summary: "A window handles the platform. A graphics device handles GPU operations.",
          description: ["GLFW manages native windows and input. OpenGL implements graphics operations. A small integration layer combines them and supplies the current-context access needed by the device.", "Neither backend library depends on the other. Backend-neutral factories select implementations from their device or context. The architecture allows other backends; OpenGL is the implemented one today."],
          boundary: "A graphics context is the backend's current execution environment. It is not the same thing as a frame's convenient drawing-command handle. GPU calls must respect its thread and lifetime.",
          links: [guide("Windows and contexts", "window_and_context"), source("include/vng/glfw_opengl/glfw_opengl.hpp")]
        }
      ]
    },
    {
      id: "editor", title: "Inside the editor", kind: "Editor app",
      summary: "One process owns authoring and UI; another runs the preview and rendering.",
      description: ["The editor application lives in examples/editor. Its main process owns editing state, panels, interaction tools, and preview coordination. The worker process owns the preview runtime and its graphics resources.", "These two processes cooperate through explicit messages and shared-memory preview delivery. They are not a general-purpose game simulation scheduler."],
      links: [guide("Editor", "editor"), guide("Editor boundaries", "editor_boundaries"), source("examples/editor/app.cpp"), source("examples/editor_worker.cpp")],
      children: [
        {
          id: "editing-session", title: "EditingSession — the authoring owner", kind: "Editor app",
          summary: "Owns the document, private viewing state, active edit, history, clipboard, and persistence.",
          description: ["Tools read the session's state and submit typed edits to it. One gesture begins, updates while you drag, and then commits or cancels. Commit creates one undo entry; cancellation restores the starting values.", "The session emits descriptions of what changed. The parent application routes these to preview synchronization. The session itself does not depend on widgets, worker transport, or OpenGL."],
          api: "auto started = session.begin_move(primary_id, selected_ids);\n// During the drag:\nauto changed = session.move(new_position);\n// On release (or use cancel() on Escape):\nauto committed = session.commit();",
          links: [guide("Editing session", "editing_session"), source("examples/editor/editing_session.hpp")],
          children: [
            {
              id: "document", title: "Document — authored content", kind: "Editor app",
              summary: "The scene you are making, rather than the way you happen to be looking at it.",
              description: ["The document contains applied blueprint geometry, separate mesh drafts, scene instances (including scene cameras), animation, and world bounds. Content edits affect its revision and saved status.", "The session owns it. It is not a renderer, and it does not own GPU buffers."],
              related: ["blueprints", "mesh-drafts", "timeline"], links: [source("examples/editor/project.hpp")]
            },
            {
              id: "viewport", title: "ViewportState — private viewing state", kind: "Editor app",
              summary: "Navigation, active inspection target, view mode, and playback position.",
              description: ["Orbiting the private editor camera changes your view, not the authored scene. The application also tracks selection sets locally; panels and component tools own their specific selections.", "Scroll moves camera is enabled by default: wheel / Ctrl+middle-drag moves forward or backward while preserving optical zoom. Turn it off in Camera settings to change magnification instead. Orbit distance and the lens remain separate values.", "Scene cameras are instances; Enter one, navigate, and Save this camera to author the view. That is an authored change and follows keyframe rules; Inspect only looks through a camera. A presented preview image carries the actual camera and frame identity used to render it."],
              boundary: "Viewport updates have their own sequence. Private camera navigation does not create document revisions, undo entries, or mesh uploads.",
              related: ["camera", "preview"], links: [guide("State and interaction timing", "editor_boundaries")]
            },
            {
              id: "history", title: "Transactions, undo & saving", kind: "Editor app",
              summary: "A drag can update immediately while remaining one undoable action.",
              description: ["The session snapshots the values an edit needs to restore, rather than cloning the whole scene for every mouse movement. Structural changes may need broader snapshots.", "Saving writes through the session's scene-file boundary. Undo and redo restore authored changes without rewinding unrelated private camera navigation."],
              related: ["edit-a-mesh", "preview"], links: [guide("Editing session", "editing_session")]
            }
          ]
        },
        {
          id: "authoring-model", title: "What you can author", kind: "Editor app",
          summary: "Shared blueprints, individual instances, mesh drafts, animation, and annotation regions.",
          description: ["These concepts describe scene data and editing policies. They are not separate global managers that all need to own each other."],
          children: [
            {
              id: "blueprints", title: "Blueprints versus instances", kind: "Concept",
              summary: "One reusable definition; many individually placed objects.",
              description: ["A mesh blueprint provides shared geometry. A scene instance refers to that blueprint and adds its own identity, transform, and settings. Scaling a spaceship instance does not resize the shared spaceship mesh.", "Deleting an instance leaves the blueprint available for reuse. A region uses a blueprint as a starting shape but owns its boundary per instance."],
              related: ["batching", "regions", "mesh-drafts"], links: [guide("Editor", "editor"), source("examples/editor/project.hpp")]
            },
            {
              id: "mesh-drafts", title: "Mesh drafts, Apply, and Save", kind: "Editor app",
              summary: "Editing a blueprint is not the same as publishing it into the scene.",
              description: ["The first mesh edit creates a draft for that blueprint. Mesh view shows that draft; scene instances continue using the applied geometry.", "Apply publishes the draft in memory. Save writes the applied and draft versions separately to disk; it does not secretly publish unfinished work."],
              related: ["edit-a-mesh", "document"], links: [guide("Mesh draft boundaries", "editor_boundaries")]
            },
            {
              id: "timeline", title: "Timeline & keyframes", kind: "Editor app",
              summary: "Values are attached to timestamps, with an explicit choice about interpolation.",
              description: ["A keyframe describes values at a time measured in seconds, not at an assumed display frame number. A key's incoming interpolation controls how the previous value approaches it.", "Time zero is always an editable initial pose. Pose edits require a paused keyframe; between keys or after the last key, insert a keyframe first. Inserting samples the scene as it currently looks."],
              boundary: "Blueprint geometry, annotation boundaries, world bounds, and private navigation are not ordinary animated pose edits. The keyframe panel shows changed items plus the current selection or unsent drafts.",
              links: [guide("Timeline", "timeline"), guide("Editor rules", "editor_boundaries"), source("examples/editor/keyframes.cpp")]
            },
            {
              id: "regions", title: "Regions — editable TODO volumes", kind: "Editor app",
              summary: "Mark a place in the scene and reshape its boundary right there.",
              description: ["A region is an ordinary instance of the region blueprint, with its own boundary, note, visibility, and transform. Its vertices and faces are internal components, not separate scene instances.", "Region tools allow concave or unfinished shapes. They are editor annotation cages, not guaranteed watertight collision volumes, and they do not appear in independent Play."],
              related: ["interaction", "blueprints"], links: [guide("Regions", "regions"), source("examples/editor/region_editor.cpp")]
            }
          ]
        },
        {
          id: "interaction", title: "Panels, selection & gizmos", kind: "Editor app",
          summary: "The UI describes controls; tools turn pointer gestures into explicit session edits.",
          description: ["A gizmo is an on-screen manipulation handle, such as an axis arrow or rotation ring. Blueprint descriptions can add meaningful controls, such as a ship's forward/back axis.", "The first top bar has Open scene, Save/Save As, Import mesh, Undo/Redo, Logs, Settings and playback/window actions. Open scene browses for a .vscene project, like Import browses for assets. The second bar groups View, Camera settings, world-bounds/region controls and debug display. More... and Tools... hold overflow at narrow widths; the sidebar keeps Scene, Keyframe values and Instance properties tabs.", "ViewportInteraction coordinates gesture capture, tool priority, and cancellation. Selection stays local; it does not rebuild the timeline or serialize the entire scene. Picking uses mesh acceleration structures to avoid testing every face unnecessarily."],
          boundary: "For a multiple-object selection, the gizmo list exposes common supported tools. Region component editing works on internal geometry without inventing extra scene instances.",
          related: ["editing-session", "regions", "ui"], links: [guide("Interaction boundaries", "editor_boundaries"), source("examples/editor/viewport_interaction.hpp")]
        },
        {
          id: "preview", title: "Preview communication & presentation", kind: "Editor app",
          summary: "Deliver changes to the worker, then display the completed image with its metadata.",
          description: ["Document changes travel as explicit property or vertex patches where possible; structural changes can need a full snapshot. PreviewUpdates combines pending changes while a worker update is in flight instead of queuing every historical mouse movement.", "Private viewing updates use a separate route. Presentation accepts completed frames with the matching generation, revision, viewing information, and sampled time. This prevents an old image from being mistaken for a newer edit."],
          boundary: "This layer delivers changes and images. It does not own the authoring rules or the GPU resources used to render them.",
          related: ["worker", "viewport", "move-an-instance"], links: [guide("Preview delivery", "editor_boundaries"), source("examples/editor/preview_updates.hpp"), source("examples/editor/preview_updates.cpp")]
        },
        {
          id: "worker", title: "Worker & rendering runtime", kind: "Editor app",
          summary: "Owns the preview's GPU resources and renders the requested scene state.",
          description: ["The worker runs effect code, samples playback, and renders on its graphics-context-owning thread. Its Runtime owns per-blueprint GPU realizations, render targets, and shader products.", "Embedded preview returns images to the UI. Independent Play presents from a worker-owned window to avoid the embedded-preview delivery cost. Rebuild/restart isolates reloadable effect execution from the UI process."],
          boundary: "Two processes do not imply separate rendering and game-logic threads inside the worker. The demo playback path is not a general physics or simulation system.",
          related: ["preview", "batching", "window-device"], links: [guide("Editor processes", "editor_boundaries"), source("examples/editor_worker.cpp"), source("examples/editor/runtime.cpp")]
        }
      ]
    },
    {
      id: "follow-the-data", title: "Follow one thing through the system", kind: "Walkthrough",
      summary: "Short journeys that connect the pieces without opening every branch.",
      description: ["Read these after the big picture. Each sequence describes one concrete path, not a new layer of abstractions."],
      children: [
        {
          id: "draw-a-frame", title: "From a mesh file to an image", kind: "Walkthrough",
          summary: "Load → upload → prepare shaders → choose state → draw → present.",
          steps: ["Load a .vmesh file through a schema into a typed CPU mesh.", "Upload its geometry to a GPU mesh. Keep that resource for later frames.", "Build shader IR once and compile a backend Program. This is setup work, not a per-frame step.", "Begin a frame and create a RenderView from the camera and image size.", "Set the draw's graphics settings, run the program with current arguments, supply the view, and draw the mesh.", "End the frame and present the window. A renderer can own this policy, but the direct demo shows every step without one."],
          related: ["files", "shader-dsl", "frame", "renderer"], links: [source("examples/file_mesh_direct.cpp"), source("examples/file_mesh.cpp")]
        },
        {
          id: "move-an-instance", title: "When you drag a spaceship", kind: "Walkthrough",
          summary: "The edit is local and immediate; the preview catches up without resending its mesh.",
          steps: ["A gizmo begins an edit transaction through EditingSession.", "Pointer movement updates the selected instance's transform locally. Shared blueprint geometry stays unchanged.", "The session reports the affected properties. PreviewUpdates coalesces those changes into the worker delivery path.", "The worker updates instance values, renders the next image, and returns its identity and camera metadata.", "Releasing the pointer commits one undo action. Escape cancels the gesture and sends the restoring change."],
          boundary: "Editing an animated pose requires a paused keyframe. Moving the private viewing camera uses viewport updates instead of this authored-edit path.",
          related: ["editing-session", "preview", "timeline"], links: [source("examples/editor/translation_tool.cpp"), guide("Editing session", "editing_session")]
        },
        {
          id: "edit-a-mesh", title: "When you edit, apply, and save a mesh", kind: "Walkthrough",
          summary: "A working draft, a published shape, and a file on disk have different jobs.",
          steps: ["Choose the blueprint's mesh view and edit its draft vertices or topology.", "The isolated mesh preview shows the draft. Switching to Scene still shows applied geometry.", "Apply publishes that blueprint's draft so scene instances use the new shape.", "Save persists authored content, including drafts, without implicitly applying other unfinished drafts."],
          related: ["mesh-drafts", "blueprints", "history"], links: [guide("Editor mesh boundaries", "editor_boundaries")]
        },
        {
          id: "diagnostics", title: "When the rendered image looks wrong", kind: "Walkthrough",
          summary: "Inspect an enhanced rendering of the same shader—not a hand-maintained debug copy.",
          description: ["The shader runtime emits additional shader variants from the same IR to record color, depth, surface identity, or explicitly requested observations. Evidence connects pixels back to source geometry.", "Frame-based captures snapshot current managed graphics settings. A concrete renderer can instead supply the same explicit raster policy it uses for its draw. Diagnostic sweeps change one tested setting, such as culling, to help explain missing surfaces."],
          boundary: "Capture is opt-in and currently synchronous, opaque, and single-mesh/single-instance. It is not a full-scene debugger for arbitrary transparent effects. Unsupported raster modes are rejected, and native state is restored after capture.",
          related: ["shader-dsl", "graphics-state"], links: [guide("Analysis rendering", "analysis_rendering"), guide("Evidence and inspection", "analysis_evidence"), source("include/vng/render_opengl/program_runtime.hpp")]
        }
      ]
    }
  ];
})();
