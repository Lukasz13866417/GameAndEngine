#pragma once

#include <variant>

#include <vng/content/diagnostic.hpp>
#include <vng/gfx/camera.hpp>
#include <vng/opengl/diagnostic.hpp>
#include <vng/shader/diagnostic.hpp>
#include <vng/window/glfw.hpp>

namespace example {

struct UsageError;

// Startup crosses two independently typed failure domains. Keep the original
// diagnostic intact while giving demos one result type for session creation.
using StartupDiagnostic = std::variant<
    vng::window::Diagnostic,
    vng::opengl::Diagnostic>;

[[nodiscard]] int fail(const UsageError& error);
[[nodiscard]] int fail(const vng::content::Diagnostic& diagnostic);
[[nodiscard]] int fail(const vng::shader::Diagnostic& diagnostic);
[[nodiscard]] int fail(const vng::opengl::Diagnostic& diagnostic);
[[nodiscard]] int fail(const vng::window::Diagnostic& diagnostic);
[[nodiscard]] int fail(const vng::gfx::CameraDiagnostic& diagnostic);

template<class... Diagnostics>
[[nodiscard]] int fail(const std::variant<Diagnostics...>& diagnostic)
{
    return std::visit(
        [](const auto& value) { return fail(value); }, diagnostic);
}

} // namespace example
