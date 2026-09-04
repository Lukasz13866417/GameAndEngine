#include <vng/shader/shader.hpp>

#include <utility>

auto bypass_link(
    vng::shader::ShaderStage vertex,
    vng::shader::ShaderStage fragment)
{
    return vng::shader::GraphicsProgram{
        std::move(vertex),
        std::move(fragment),
    };
}
