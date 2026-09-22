#include "scenes/scene_demo.hpp"

int main(int argc,char** argv) {
    constexpr std::array evidence{
        example::fleet::EvidenceSubject{1,"pathfinder-faces.png"},
        example::fleet::EvidenceSubject{3,"flagship-faces.png"},
        example::fleet::EvidenceSubject{2,"sun-faces.png"}};
    return example::run_scene_demo(argc,argv,{"vng_fleet_reveal_demo",VNG_FLEET_SCENE_PATH,
        "HELIOS / beyond the limb — fleet rendezvous",24,evidence});
}
