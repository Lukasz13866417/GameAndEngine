#include "fleet_inspection.hpp"
#include "../support/presentation.hpp"
#include "../editor/animation.hpp"
#include <vng/analysis/manifest.hpp>
#include <sstream>

namespace example::fleet {
vng::resources::Result<void> inspect(vng::opengl::Device& device,
    editor_example::Runtime& runtime, editor_example::State& state,
    const vng::gfx::Camera& camera, vng::Extent2D extent, vng::f32 time,
    const std::filesystem::path& directory, std::span<const EvidenceSubject> subjects) {
    namespace project = editor_example;
    using namespace vng;
    std::error_code error;
    if (!std::filesystem::create_directory(directory,error)) {
        resources::Diagnostic diagnostic;
        diagnostic.message = "Analysis requires a new directory: " + directory.string() +
                             (error ? " / " + error.message() : " / already exists");
        return std::unexpected(std::move(diagnostic));
    }
    std::ostringstream summary;
    summary << "Scene analysis, sampled time " << time << " s\n"
        << "composite.png: full scene, actual shared-depth occlusion and postprocessing.\n"
        << "*-faces.png: enhanced-shader source-face views of isolated selected objects;\n"
        << "these are NOT scene-wide visibility masks.\n\n";
    auto normal = runtime.render(device,{state,camera,extent,time,false});
    if (!normal) return std::unexpected(normal.error());
    auto pixels = std::span<const u8>(reinterpret_cast<const u8*>(normal->pixels.data()),normal->pixels.size());
    if (auto saved = write_rgba8_png(directory/"composite.png",extent,pixels); !saved) return saved;
    auto snapshot = camera.snapshot(extent);
    if (snapshot) summary << "Camera position: " << snapshot->position.x << ", " << snapshot->position.y
                          << ", " << snapshot->position.z << '\n';
    const auto original_selection = state.viewport.selected_object;
    struct Restore { project::State& state; u32 selection; ~Restore() { state.viewport.selected_object=selection; } }
        restore{state,original_selection};
    for (const auto& [id,name] : subjects) {
        if (!project::find_instance(state,id)) continue;
        state.viewport.selected_object = id;
        auto image = runtime.render(device,{state,camera,extent,time,true});
        if (!image) return std::unexpected(image.error());
        pixels = {reinterpret_cast<const u8*>(image->pixels.data()),image->pixels.size()};
        if (auto saved = write_rgba8_png(directory/name,extent,pixels); !saved) return saved;
        summary << "\n" << name << ": " << runtime.diagnostics() << '\n';
        if (const auto* manifest = runtime.diagnostic_manifest())
            for (const auto& item : manifest->items())
                summary << "  item " << item.id.value << " / mesh " << item.provenance.mesh.value
                        << " / source faces " << item.primitives->size() << '\n';
    }
    summary << "\nSampled instances (blueprints remain separate from these transforms):\n";
    for (const auto& instance : state.document.instances) {
        const auto sample = project::evaluate_instance(state,instance,time);
        const auto p = sample.transform.position;
        summary << sample.id << " / " << sample.name << " / blueprint " << static_cast<u32>(sample.blueprint)
            << " / position " << p.x << ", " << p.y << ", " << p.z
            << " / scale " << sample.transform.scale << '\n';
    }
    return resources::into_result(project::save_new(directory/"summary.txt",summary.str()));
}
} // namespace example::fleet
