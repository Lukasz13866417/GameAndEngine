#pragma once

#include "presentation.hpp"

#include <filesystem>

#include <vng/analysis/diagnosis.hpp>
#include <vng/core/types.hpp>
#include <vng/opengl/device.hpp>
#include <vng/resources/diagnostic.hpp>

namespace example::spaceflight {

using example::save_screenshot;

[[nodiscard]] vng::resources::Result<void> export_diagnostics(
    const vng::analysis::DiagnosticSweep& sweep,
    const std::filesystem::path& directory);

} // namespace example::spaceflight
