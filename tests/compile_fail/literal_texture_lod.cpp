#include <vng/shader/shader.hpp>

using Context = vng::shader::StageContext<vng::shader::StageKind::vertex,
    vng::shader::VertexInputs<>,
    vng::shader::VertexOutputs<vng::shader::ClipPosition>>;

auto rejected(Context& stage, vng::dsl::Float2 coordinates, float lod)
{
    return stage.sample_2d_lod<0>(coordinates, lod);
}
