#pragma once
#include <filesystem>
#include <optional>
namespace editor_example {
class Automation;
struct Options {
    std::optional<std::filesystem::path> scene, mesh, screenshot;
    bool once{}, diagnostic{}, mesh_view{}, play{};
    bool debug_link{true};
    bool windowed{}, fullscreen{}, settings{}, import_dialog{};
    // In-process automation drives the real UI/worker without desktop input.
    // Supply a private settings path so a test never reads/writes user prefs.
    Automation* automation{};
    std::optional<std::filesystem::path> preferences;
};
int run(const Options&);
} // namespace editor_example
