#include "scenes/scene_demo.hpp"
#include "scenes/asteroid_scene.hpp"

int main(int argc,char** argv) {
    constexpr std::array evidence{
        example::fleet::EvidenceSubject{1,"pathfinder-faces.png"},
        example::fleet::EvidenceSubject{3,"flagship-faces.png"},
        example::fleet::EvidenceSubject{20,"exit-rock-faces.png"}};
    return example::run_scene_demo(argc,argv,{"vng_asteroid_fleet_demo",VNG_ASTEROID_SCENE_PATH,
        "SHATTERED REACH / through the belt",example::asteroids::reveal_time,evidence});
}
