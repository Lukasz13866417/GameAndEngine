#include <vng/shader/shader.hpp>

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Tint : vng::gfx::Semantic<vng::Vec4> {};

using Inputs = vng::shader::VertexInputs<Position, Tint>;
using Outputs = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<Tint>>;

auto force_instantiation = vng::shader::vertex<Inputs, Outputs>(
    [](auto& stage) {
        return stage.output(
            vng::dsl::field<vng::shader::ClipPosition>(
                stage.input(Position{})),
            vng::dsl::field<Tint>(stage.input(Tint{})));
    });
