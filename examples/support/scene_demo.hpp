#pragma once
#include "fleet_inspection.hpp"
#include <array>

namespace example {
struct SceneDemo {
    const char* name;
    std::filesystem::path scene;
    const char* title;
    vng::f32 capture_time;
    std::span<const fleet::EvidenceSubject> evidence;
};
// Shared playback/presentation only; scene content lives in ordinary .vscene files.
int run_scene_demo(int argc,char** argv,const SceneDemo&);
}
