#pragma once

#include <vng/shader/diagnostic.hpp>
#include <vng/shader/stage.hpp>

#include <string>

namespace vng::shader {

class GraphicsProgram {
public:
    [[nodiscard]] const ShaderStage& vertex() const noexcept { return vertex_; }
    [[nodiscard]] const ShaderStage& fragment() const noexcept { return fragment_; }

    [[nodiscard]] std::string dump_interface() const;

private:
    GraphicsProgram(ShaderStage vertex, ShaderStage fragment)
        : vertex_(std::move(vertex)), fragment_(std::move(fragment)) {}

    ShaderStage vertex_;
    ShaderStage fragment_;

    friend Result<GraphicsProgram> link(ShaderStage vertex, ShaderStage fragment);
};

[[nodiscard]] Result<GraphicsProgram> link(ShaderStage vertex, ShaderStage fragment);

} // namespace vng::shader
