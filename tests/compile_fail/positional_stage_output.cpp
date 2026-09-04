#include <vng/shader/shader.hpp>

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Tint : vng::gfx::Semantic<vng::Vec4> {};

using Inputs = vng::shader::VertexInputs<Position, Tint>;
using Outputs = vng::shader::VertexOutputs<
    vng::shader::ClipPosition,
    vng::shader::smooth<Tint>>;

auto force_instantiation = vng::shader::vertex<Inputs, Outputs>(
    [](auto& stage) {
        auto position = stage.input(Position{});
        auto clip = vng::dsl::vec4(position, 0.0F, 1.0F);
        return stage.output(clip, stage.input(Tint{}));
    });
