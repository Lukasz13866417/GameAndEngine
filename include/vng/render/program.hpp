#pragma once

namespace vng::shader { class GraphicsProgram; }

namespace vng::render {

// Backend selection follows the concrete device through ADL. Compilation
// owns shader code and interface metadata, independently of graphics state.
struct CompileProgram final {
    template<class Device, class Program>
    [[nodiscard]] auto operator()(
        const Device& device, const Program& program) const
        -> decltype(compile_graphics_program(device, program))
    {
        return compile_graphics_program(device, program);
    }
};
inline constexpr CompileProgram compile_program{};

} // namespace vng::render
