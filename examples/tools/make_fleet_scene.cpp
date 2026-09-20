#include "../support/fleet_scene.hpp"
#include "../editor/scene_file.hpp"
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    const bool replace = argc == 4 && std::string_view(argv[3]) == "--replace";
    if (argc != 3 && !replace) {
        std::cerr << "Usage: vng_make_fleet_scene ASSET_DIRECTORY SCENE.vscene [--replace]\n"
            << "--replace discards edits to that generated scene; omitted by default.\n";
        return 1;
    }
    auto state = example::fleet::author_scene(argv[1]);
    if (!state) { std::cerr << state.error().message << '\n'; return 1; }
    editor_example::SceneFile file;
    if (auto saved = file.save_as(argv[2],*state,replace); !saved) {
        std::cerr << saved.error().message << '\n'; return 1;
    }
    std::cout << "Authored " << state->document.instances.size()-1 << " ships / "
              << state->document.mesh_assets.size()+1 << " mesh blueprints / "
              << state->document.timeline_duration << " seconds into " << argv[2] << '\n';
}
