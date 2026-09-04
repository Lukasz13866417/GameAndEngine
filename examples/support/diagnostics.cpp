#include "diagnostics.hpp"

#include "options.hpp"

#include <iostream>

namespace example {

int fail(const UsageError& error)
{
    std::cerr << error.message << '\n';
    return 2;
}

int fail(const vng::content::Diagnostic& diagnostic)
{
    std::cerr << "content: " << diagnostic.message;
    if (!diagnostic.path.empty()) {
        std::cerr << " [" << diagnostic.path.string() << ']';
    }
    if (diagnostic.location) {
        std::cerr << " at " << diagnostic.location->line
                  << ':' << diagnostic.location->column;
    }
    std::cerr << '\n';
    for (const auto& note : diagnostic.notes) {
        std::cerr << "  " << note << '\n';
    }
    return 1;
}

int fail(const vng::shader::Diagnostic& diagnostic)
{
    std::cerr << "shader pipeline: " << diagnostic.message << '\n';
    for (const auto& note : diagnostic.notes) {
        std::cerr << "  " << note << '\n';
    }
    if (!diagnostic.generated_source.empty()) {
        std::cerr << diagnostic.generated_source << '\n';
    }
    return 1;
}

int fail(const vng::opengl::Diagnostic& diagnostic)
{
    std::cerr << "OpenGL: " << diagnostic.message << '\n';
    if (!diagnostic.driver_log.empty()) {
        std::cerr << diagnostic.driver_log << '\n';
    }
    if (!diagnostic.generated_source.empty()) {
        std::cerr << diagnostic.generated_source << '\n';
    }
    return 1;
}

int fail(const vng::window::Diagnostic& diagnostic)
{
    std::cerr << "window: " << diagnostic.message;
    if (diagnostic.native_code != 0) {
        std::cerr << " (native error " << diagnostic.native_code << ')';
    }
    std::cerr << '\n';
    return 1;
}

int fail(const vng::gfx::CameraDiagnostic& diagnostic)
{
    std::cerr << "camera: " << diagnostic.message << '\n';
    return 1;
}

} // namespace example
