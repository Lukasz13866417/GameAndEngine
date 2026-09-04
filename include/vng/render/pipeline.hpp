#pragma once

#include <utility>

#include <vng/render/color.hpp>

namespace vng::shader { class GraphicsProgram; }

namespace vng::render {

// Backend-neutral depth comparison. Backends translate these values when a
// GraphicsPipelineDesc is realized for a device.
enum class DepthCompare {
    never,
    less,
    less_equal,
    equal,
    greater_equal,
    greater,
    not_equal,
    always,
};

struct DepthState final {
    bool test{false};
    bool write{false};
    DepthCompare compare{DepthCompare::less};

    friend constexpr bool operator==(const DepthState&, const DepthState&)
        = default;
};

enum class CullMode {
    none,
    front,
    back,
};

enum class FrontFace {
    clockwise,
    counter_clockwise,
};

// A portable request for graphics-pipeline behavior. It owns no backend
// resources and contains no OpenGL/Vulkan/etc. values. A backend compiles it
// into its own persistent pipeline representation.
struct GraphicsPipelineDesc final {
    DepthState depth{};
    CullMode cull{CullMode::none};
    FrontFace front_face{FrontFace::counter_clockwise};

    ColorEncoding output_encoding{ColorEncoding::srgb};

    friend constexpr bool operator==(
        const GraphicsPipelineDesc&,
        const GraphicsPipelineDesc&) = default;
};

// User-facing backend-selecting compilation point. The shader program is the
// linked, backend-neutral IR program; the concrete Device finds its backend's
// compile_graphics_pipeline overload through ADL. Consequently this header
// does not include or enumerate any graphics backend.
struct CompilePipeline final {
    template<class Device>
    [[nodiscard]] constexpr auto operator()(
        const Device& device,
        const shader::GraphicsProgram& program,
        GraphicsPipelineDesc description = {}) const
        noexcept(noexcept(compile_graphics_pipeline(
            device,
            program,
            description)))
        -> decltype(compile_graphics_pipeline(
            device,
            program,
            description))
    {
        return compile_graphics_pipeline(
            device,
            program,
            description);
    }
};

inline constexpr CompilePipeline compile_pipeline{};

namespace expert {

// Low-level escape hatch for backend integrations and tooling that already
// owns a native/backend program. Ordinary application code should pass its
// shader::GraphicsProgram to render::compile_pipeline instead.
struct RealizePipeline final {
    template<class Device, class BackendProgram>
    [[nodiscard]] constexpr auto operator()(
        const Device& device,
        BackendProgram&& program,
        GraphicsPipelineDesc description = {}) const
        noexcept(noexcept(realize_graphics_pipeline(
            device,
            std::forward<BackendProgram>(program),
            description)))
        -> decltype(realize_graphics_pipeline(
            device,
            std::forward<BackendProgram>(program),
            description))
    {
        return realize_graphics_pipeline(
            device,
            std::forward<BackendProgram>(program),
            description);
    }
};

inline constexpr RealizePipeline realize_pipeline{};

} // namespace expert

} // namespace vng::render
