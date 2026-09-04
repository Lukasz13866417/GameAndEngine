#include <vng/opengl/gpu_mesh.hpp>

struct Position : vng::gfx::Semantic<vng::Vec3> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};

using Vertex = vng::gfx::Record<Position>;
using Inputs = vng::shader::VertexInputs<Position, Color>;

void bind_incomplete_mesh(
    vng::opengl::GpuMesh<Vertex>& mesh,
    const vng::opengl::Device& device)
{
    (void)mesh.prepare_vertex_input<Inputs>(device);
}
