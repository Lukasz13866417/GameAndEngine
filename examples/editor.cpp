#include "editor/app.hpp"
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    editor_example::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help") {
            std::cout
                << "Vibe editor: --scene FILE | --mesh FILE, --mesh-view, --diagnostic, --once, "
                   "--screenshot NEW.png, --play, --no-debug-link, --windowed, --fullscreen, --settings, --import-dialog\n"
                   "Default: maximized with borders. F11 toggles true fullscreen.\n"
                   "Edit examples/editor/effects.cpp or runtime.cpp, then Reload C++.\n"
                   "Viewport: MMB orbit, Shift+MMB pan, wheel or Ctrl+MMB zoom.\n"
                   "Scene cameras: select one, then Inspect/Enter to look through it; Save this camera authors the view.\n"
                   "Objects mode: click a surface to select; Tab switches mesh vertex editing.\n"
                   "Delete removes the selected keyframe or object instance; blueprints remain.\n"
                   "Import mesh... browses .vmesh files and adds a blueprint + instance without replacing the scene.\n"
                   "Click a blueprint to create another instance. Undo restores deletions.\n"
                   "Settings controls FPS caps and preview resolution independently of scene data.\n"
                   "Numeric fields: type then Enter to commit; Escape cancels the draft.\n"
                   "Save / Ctrl+S updates the current scene; Save As / Ctrl+Shift+S chooses a "
                   "file.\n"
                   "Save As confirms replacement. Mesh exports create new files only.\n"
                   "Play (in editor) animates the viewport; Play (independent) opens the worker "
                   "window.\n";
            return 0;
        }
        if (arg == "--once")
            options.once = true;
        else if (arg == "--diagnostic")
            options.diagnostic = true;
        else if (arg == "--mesh-view")
            options.mesh_view = true;
        else if (arg == "--play")
            options.play = true;
        else if (arg == "--windowed")
            options.windowed = true;
        else if (arg == "--fullscreen")
            options.fullscreen = true;
        else if (arg == "--settings")
            options.settings = true;
        else if (arg == "--import-dialog")
            options.import_dialog = true;
        else if (arg == "--no-debug-link")
            options.debug_link = false;
        else if ((arg == "--scene" || arg == "--mesh" || arg == "--screenshot") && i + 1 < argc) {
            auto path = std::filesystem::path{argv[++i]};
            if (arg == "--scene")
                options.scene = std::move(path);
            else if (arg == "--mesh")
                options.mesh = std::move(path);
            else
                options.screenshot = std::move(path);
        } else {
            std::cerr << "Unknown or incomplete option: " << arg << '\n';
            return 1;
        }
    }
    try {
        return editor_example::run(options);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
