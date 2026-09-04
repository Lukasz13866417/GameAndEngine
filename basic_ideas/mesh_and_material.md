# Mesh
A mesh is the result of manually entering (in code) or loading from disk (or from an editor) some vertices, edges and faces. Other per-vertex stuff can be specified too. There will be multiple file formats, so multiple parsers also. My biggest uncertainty is how much parsing control this custom per-vertex (and possibly other custom) stuff should have. Like, make the format describe the division into vertex attribs, and the vertex format will simply define the order and deciphring of textual descriptions of vertex attribs? Or a different way? 

## First implemented decision

The first format is a strict textual source format, `.vmesh` 1.0. It describes
named logical fields and values; it does not describe packed GPU encodings or
copy native C++ layout. A reusable schema maps stable file names to semantic
types, then `Record::set` chooses the actual runtime codec. This keeps format
parsing, semantic meaning, and backend storage separate.

The runtime `Mesh<Record...>` owns one or more synchronized vertex streams, one
triangle-face list, and an optional explicit edge list. Multiple streams share
one index domain. Instance streams do not belong to a mesh. The initial format
is render-ready rather than a full DCC interchange representation; polygons and
per-corner data should be handled by a later importer before this boundary.

The ordinary OpenGL boundary is one explicit immutable upload:
`upload_mesh(device, mesh)`. The resulting `GpuMesh` owns its vertex/index
buffers and lazily caches VAOs from the actual linked program's semantic input
contract. Stream resolution remains inspectable as a low-level mechanism, but
is not part of normal mesh drawing.

See `docs/mesh_and_vmesh.md` for the exact syntax and API.
# Material
A material is probably just all the info necessary and sufficient to be broken down into the full shader stuff from a given backend. But this translation will happen in the backend-dependent renderer. So the material is mostly a spec. 
