#include "../scenes/tunnel_departure.hpp"
#include "../editor/scene_file.hpp"
#include <iostream>
#include <string_view>

int main(int argc,char** argv) {
    const bool replace=argc==4 && std::string_view(argv[3])=="--replace";
    if(argc!=3 && !replace) {
        std::cerr<<"Usage: vng_make_tunnel_departure ASSET_DIRECTORY SCENE.vscene [--replace]\n";return 1;
    }
    auto scene=example::tunnel::departure::author_scene(argv[1]);
    if(!scene){std::cerr<<scene.error().message<<'\n';return 1;}
    editor_example::SceneFile file;
    if(auto saved=file.save_as(argv[2],*scene,replace);!saved){std::cerr<<saved.error().message<<'\n';return 1;}
    std::cout<<"Authored SKYWAY / express departure: "<<scene->document.instances.size()<<" instances / "
             <<scene->document.timeline_duration<<" seconds\n";
}
