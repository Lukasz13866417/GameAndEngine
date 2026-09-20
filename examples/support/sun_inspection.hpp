#pragma once
#include <filesystem>
#include <vng/analysis/diagnosis.hpp>
#include <vng/resources/diagnostic.hpp>

namespace example::sun {
[[nodiscard]] vng::resources::Result<void> export_diagnostics(
    const vng::analysis::DiagnosticSweep&, const std::filesystem::path&,
    vng::Vec3 center = {});
}
