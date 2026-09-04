#include <vng/shader/shader.hpp>

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Missing : vng::gfx::Semantic<vng::Vec2> {};

using Inputs = vng::shader::VertexInputs<Position>;
using Outputs = vng::shader::VertexOutputs<vng::shader::ClipPosition>;

auto force_instantiation = vng::shader::vertex<Inputs, Outputs>(
    [](auto& stage) {
        auto missing = stage.input(Missing{});
        return stage.output(
            vng::dsl::field<vng::shader::ClipPosition>(
                vng::dsl::vec4(missing, 0.0F, 1.0F)));
    });
