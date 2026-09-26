#include "scenes/scene_demo.hpp"

int main(int argc,char** argv) {
    // The tunnel is part of the Earth mesh (instance 4); the courier is 2.
    constexpr std::array evidence{example::fleet::EvidenceSubject{4,"tunnel-faces.png"},
        example::fleet::EvidenceSubject{2,"courier-faces.png"}};
    return example::run_scene_demo(argc,argv,{"vng_tunnel_demo",VNG_TUNNEL_SCENE_PATH,
        "SKYWAY / express departure",7.F,evidence});
}
