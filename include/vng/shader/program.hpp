#pragma once

#include <vng/shader/diagnostic.hpp>
#include <vng/shader/stage.hpp>

#include <string>

namespace vng::shader {

class GraphicsProgram;
namespace detail {
Result<GraphicsProgram> link_stages(ShaderStage vertex, ShaderStage fragment, bool shared);
class TypedProgramAccess;
}

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
    friend Result<GraphicsProgram> detail::link_stages(ShaderStage, ShaderStage, bool);
};

[[nodiscard]] Result<GraphicsProgram> link(ShaderStage vertex, ShaderStage fragment);

// The neutral implementation stays erased; this thin owning facade carries
// the CPU call signature through link and backend compilation, without casts.
template<Argument... Args>
class TypedGraphicsProgram final {
public:
    using signature = Arguments<Args...>;
    [[nodiscard]] const GraphicsProgram& untyped() const & noexcept { return program_; }
    const GraphicsProgram& untyped() const && = delete;
    [[nodiscard]] GraphicsProgram release_untyped() && noexcept { return std::move(program_); }
    [[nodiscard]] const ShaderStage& vertex() const noexcept { return program_.vertex(); }
    [[nodiscard]] const ShaderStage& fragment() const noexcept { return program_.fragment(); }
    [[nodiscard]] std::string dump_interface() const { return program_.dump_interface(); }

private:
    explicit TypedGraphicsProgram(GraphicsProgram program) : program_(std::move(program)) {}
    GraphicsProgram program_;
    friend class detail::TypedProgramAccess;
};

namespace detail {
class TypedProgramAccess final {
public:
    template<Argument... Args>
    static Result<TypedGraphicsProgram<Args...>> finish(Result<GraphicsProgram> program)
    {
        if (!program) return std::unexpected(std::move(program.error()));
        const std::array<std::type_index, sizeof...(Args)> expected{typeid(Args)...};
        std::array<bool, sizeof...(Args)> seen{};
        auto matches = [&](const ShaderStage& stage) {
            for (const auto& parameter : stage.ir().parameters) {
                if (parameter.kind != ParameterKind::argument) continue;
                const auto index = parameter.argument_index;
                if (index >= expected.size() || parameter.argument_type != expected[index])
                    return false;
                seen[index] = true;
            }
            return true;
        };
        bool complete = matches(program->vertex()) && matches(program->fragment());
        for (const bool present : seen) complete = complete && present;
        if (!complete) {
            return std::unexpected(Diagnostic{
                .code = DiagnosticCode::interface_mismatch,
                .message = "Cannot preserve a typed signature when another stage's arguments were erased",
                .notes = {"Link typed stages together, or explicitly erase both stages."},
                .origin = {}, .generated_source = {},
            });
        }
        return TypedGraphicsProgram<Args...>{std::move(*program)};
    }
};
} // namespace detail

// Default call order is vertex arguments followed by fragment arguments.
// Equal C++ types never silently imply shared values across stages.
template<Argument... VArgs, Argument... FArgs>
[[nodiscard]] Result<TypedGraphicsProgram<VArgs..., FArgs...>> link(
    TypedShaderStage<StageKind::vertex, VArgs...> vertex,
    TypedShaderStage<StageKind::fragment, FArgs...> fragment)
{
    return detail::TypedProgramAccess::finish<VArgs..., FArgs...>(detail::link_stages(
        std::move(vertex).release_untyped(), std::move(fragment).release_untyped(), false));
}

template<Argument... Args>
[[nodiscard]] Result<TypedGraphicsProgram<Args...>> link(
    TypedShaderStage<StageKind::vertex, Args...> vertex, ShaderStage fragment)
{
    return detail::TypedProgramAccess::finish<Args...>(detail::link_stages(
        std::move(vertex).release_untyped(), std::move(fragment), false));
}

template<Argument... Args>
[[nodiscard]] Result<TypedGraphicsProgram<Args...>> link(
    ShaderStage vertex, TypedShaderStage<StageKind::fragment, Args...> fragment)
{
    return detail::TypedProgramAccess::finish<Args...>(detail::link_stages(
        std::move(vertex), std::move(fragment).release_untyped(), false));
}

template<Argument... VArgs, Argument... FArgs>
    requires std::same_as<Arguments<VArgs...>, Arguments<FArgs...>>
[[nodiscard]] Result<TypedGraphicsProgram<VArgs...>> link(
    TypedShaderStage<StageKind::vertex, VArgs...> vertex,
    TypedShaderStage<StageKind::fragment, FArgs...> fragment, SharedArguments)
{
    return detail::TypedProgramAccess::finish<VArgs...>(detail::link_stages(
        std::move(vertex).release_untyped(), std::move(fragment).release_untyped(), true));
}

} // namespace vng::shader
