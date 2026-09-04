#include <vng/shader/shader.hpp>

struct Position : vng::gfx::Semantic<vng::Vec2> {};

using Inputs = vng::shader::VertexInputs<Position>;
using Outputs = vng::shader::VertexOutputs<vng::shader::ClipPosition>;

auto force_instantiation = vng::shader::vertex<Inputs, Outputs>(
    [](auto& stage) {
        return stage.inputs().get(Position{});
    });
