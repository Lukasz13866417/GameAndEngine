#include "support/scene_demo.hpp"

int main(int argc,char** argv) {
    constexpr std::array evidence{example::fleet::EvidenceSubject{1,"earth-faces.png"}};
    return example::run_scene_demo(argc,argv,{"vng_earth_demo",VNG_EARTH_SCENE_PATH,
        "HOMEWORLD / stylized Earth",0,evidence});
}
