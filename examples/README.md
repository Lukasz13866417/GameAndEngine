# Examples

The example entry points keep engine operations visible and delegate only
presentation concerns to `support/`.

- `triangle.cpp` is the smallest end-to-end path: map a `.vmesh` schema, build
  a shader with the DSL, create the simple renderer, and submit one draw.
- `file_mesh.cpp` is the application-level walkthrough: load a 3D mesh, hand
  it to a custom renderer, optionally diagnose a frame, and render it. The
  renderer's shaders, ticket, resources, and command policy are isolated in
  `file_mesh_renderer.hpp/.cpp`; its factory owns shader creation.
- `file_mesh_direct.cpp` renders the same cube without a renderer. It keeps the
  two DSL shader stages, pipeline compilation, GPU upload, and frame commands
  visible for comparison with the renderer-owned path.
- `advanced_instanced_streams.cpp` intentionally exposes the escape hatch:
  resolve a multi-stream layout, upload raw buffers, configure a VAO, and issue
  an instanced OpenGL draw.

`support/` contains only example UI and process glue: command-line parsing,
diagnostic/report formatting, GLFW+OpenGL startup, inspection headings, and
window-loop bookkeeping. It is linked only by the demos and is not an engine
API.
