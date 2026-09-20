#include "support/scene_demo.hpp"
#include "support/solar_system_scene.hpp"

int main(int argc,char** argv) {
    constexpr std::array evidence{
        example::fleet::EvidenceSubject{example::solar_system::earth,"earth-faces.png"},
        example::fleet::EvidenceSubject{example::solar_system::first_ship,"flagship-faces.png"}};
    return example::run_scene_demo(argc,argv,{"vng_solar_system_demo",VNG_SOLAR_SYSTEM_SCENE_PATH,
        "LEAVING HOME / Earth to the belt",0,evidence});
}
