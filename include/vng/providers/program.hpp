#pragma once

#include <vng/render/program.hpp>
#include <vng/resources/resources.hpp>
#include <vng/shader/program.hpp>

namespace vng::providers {

template<class ShaderProgram>
class ProgramProvider {
public:
    explicit ProgramProvider(ShaderProgram program)
        : source_(std::make_shared<const ShaderProgram>(std::move(program))) {}
    template<class Device>
    [[nodiscard]] auto provide(Device& device) const
    { return render::compile_program(device, *source_); }
private:
    std::shared_ptr<const ShaderProgram> source_;
};
template<class ShaderProgram>
inline ProgramProvider<ShaderProgram> program(ShaderProgram program)
{ return ProgramProvider<ShaderProgram>{std::move(program)}; }

} // namespace vng::providers
