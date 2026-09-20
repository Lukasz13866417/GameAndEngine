#pragma once
#include "blueprint_mesh_renderer.hpp"
#include <map>
#include <memory>

namespace editor_example {
// Runtime owns this provider before its mesh renderers. Stable entries keep
// dependencies explicit and share one compiled pair per lighting model, never
// one program per instance. Both emission paths use mesh_shading::shade.
class MeshPrograms {
public:
    struct Entry {
        vng::opengl::Program instanced;
        mesh_shading::Program diagnostic;
    };
    vng::resources::Result<Entry*> provide(vng::opengl::Device&,mesh_shading::LightingStyle);
    Entry& at(mesh_shading::LightingStyle style) {return *entries_.at(style);}
private:
    std::map<mesh_shading::LightingStyle,std::unique_ptr<Entry>> entries_;
};
}
