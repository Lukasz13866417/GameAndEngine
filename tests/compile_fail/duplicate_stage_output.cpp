#include <vng/shader/shader.hpp>

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Tint : vng::gfx::Semantic<vng::Vec4> {};

using Inputs = vng::shader::VertexInputs<Position>;
using Outputs = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<Tint>>;

auto force_instantiation = vng::shader::vertex<Inputs, Outputs>(
    [](auto& stage) {
        auto clip = vng::dsl::vec4(stage.input(Position{}), 0.0F, 1.0F);
        return stage.output(
            vng::dsl::field<vng::shader::ClipPosition>(clip),
            vng::dsl::field<vng::shader::ClipPosition>(clip));
    });
