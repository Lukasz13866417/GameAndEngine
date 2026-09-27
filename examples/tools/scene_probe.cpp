// Answers questions about a saved scene without rendering it: what is in it,
// how close it is to the editor's limits, and where the active camera and
// one instance are at given times (screen positions on a 1280x800 frame).
#include "../editor/animation.hpp"
#include "../editor/scene_file.hpp"
#include <vng/content/document.hpp>
#include <vng/timeline/timeline.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>

namespace {
using namespace vng;
namespace project = editor_example;
int usage() {
    std::fprintf(stderr,
        "Usage: vng_scene_probe SCENE.vscene --list\n"
        "       vng_scene_probe SCENE.vscene --size\n"
        "       vng_scene_probe SCENE.vscene INSTANCE_ID TIME...\n");
    return 1;
}
const char* kind(const project::SceneInstance& instance) {
    if (std::holds_alternative<project::CameraSettings>(instance.settings)) return "camera";
    if (std::holds_alternative<project::SunSettings>(instance.settings)) return "sun";
    if (std::holds_alternative<project::RegionSettings>(instance.settings)) return "region";
    return "mesh";
}
// The document's decoded size, found by bisecting the parser's limit.
std::size_t decoded_bytes(const std::string& source) {
    std::size_t low = 1, high = std::size_t{512} << 20;
    while (high - low > 65536) {
        const auto middle = low + (high - low) / 2;
        const auto parsed = content::parse_document(source, {.limits = {.max_source_bytes = std::size_t{1} << 30,
            .max_decoded_bytes = middle, .max_string_bytes = std::size_t{512} << 20}});
        (parsed ? high : low) = middle;
    }
    return high;
}
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    project::SceneFile file;
    auto scene = file.load(argv[1]);
    if (!scene) { std::fprintf(stderr, "%s\n", scene.error().message.c_str()); return 1; }
    const auto& document = scene->document;
    const std::string_view mode = argv[2];
    if (mode == "--list") {
        for (const auto& instance : document.instances) {
            const auto blueprint = std::ranges::find(document.mesh_assets, instance.blueprint, &project::MeshBlueprint::id);
            std::printf("%6u  %-6s  %-40s  %s\n", instance.id, kind(instance), instance.name.c_str(),
                blueprint == document.mesh_assets.end() ? "" : blueprint->name.c_str());
        }
        return 0;
    }
    if (mode == "--size") {
        std::ifstream in(argv[1], std::ios::binary);
        std::stringstream buffer;buffer << in.rdbuf();
        const auto source = buffer.str();
        std::size_t keys{}, longest{};
        for (const auto& track : document.timeline.tracks()) { keys += track.keys.size(); longest = std::max(longest, track.keys.size()); }
        std::printf("file %.1f MB, decoded %.1f MiB of the editor's 64 MiB\n",
            static_cast<double>(source.size()) / 1e6, static_cast<double>(decoded_bytes(source)) / 1048576.0);
        std::printf("%zu instances, %zu blueprints, %zu tracks\n", document.instances.size(), document.mesh_assets.size(),
            document.timeline.tracks().size());
        std::printf("%zu keys of %zu; longest track %zu of %zu\n", keys, timeline::max_total_keys, longest, timeline::max_keys_per_track);
        return 0;
    }
    const auto* subject = project::find_instance(*scene, static_cast<u32>(std::atoi(argv[2])));
    if (!subject || argc < 4) return usage();
    for (int i = 3; i < argc; ++i) {
        const auto t = std::strtof(argv[i], nullptr);
        const auto* camera = project::active_camera(*scene, t);
        auto view = project::render_camera(*scene, t).snapshot({1280, 800});
        if (!camera || !view) { std::printf("t=%8.3f  no active camera\n", static_cast<double>(t)); continue; }
        const auto eye = project::evaluate_instance(*scene, *camera, t);
        const auto p = project::evaluate_instance(*scene, *subject, t).transform.position;
        std::array<double, 4> clip{};
        const std::array<double, 4> world{p.x, p.y, p.z, 1};
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned column = 0; column < 4; ++column) clip[row] += double(view->view_projection[column][row]) * world[column];
        const auto e = eye.transform.position;
        std::printf("t=%8.3f  eye (%.3f, %.3f, %.3f) forward (%.3f, %.3f, %.3f) zoom %.2f | %s (%.3f, %.3f, %.3f) "
                    "%.3f km away, screen (%.1f, %.1f)%s%s\n",
            static_cast<double>(t), double(e.x), double(e.y), double(e.z), double(view->forward.x), double(view->forward.y),
            double(view->forward.z), double(std::get<project::CameraSettings>(eye.settings).zoom), subject->name.c_str(),
            double(p.x), double(p.y), double(p.z), std::hypot(double(p.x - e.x), double(p.y - e.y), double(p.z - e.z)),
            (clip[0] / clip[3] * .5 + .5) * 1280, (1 - (clip[1] / clip[3] * .5 + .5)) * 800,
            clip[3] > 0 ? "" : " behind the camera", project::evaluate_visibility(*scene, *subject, t) ? "" : " hidden");
    }
}
