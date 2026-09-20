#include "project.hpp"
#include "animation.hpp"
#include <algorithm>

namespace editor_example {
using namespace vng;
RegionSettings* region_settings(State& state,u32 id) {
    auto* instance=find_instance(state,id);return instance?std::get_if<RegionSettings>(&instance->settings):nullptr;
}
const RegionSettings* region_settings(const State& state,u32 id) {
    const auto* instance=find_instance(state,id);return instance?std::get_if<RegionSettings>(&instance->settings):nullptr;
}
Regions region_snapshot(const State& state) {
    Regions result{state.document.next_instance_id,{}};
    for(const auto& instance:state.document.instances)if(const auto* settings=std::get_if<RegionSettings>(&instance.settings)) {
        Region r;static_cast<RegionGeometry&>(r)=settings->boundary;
        r.id=instance.id;r.name=instance.name;r.note=settings->note;r.show_walls=settings->show_walls;result.items.push_back(std::move(r));
    }
    return result;
}
Regions region_world_snapshot(const State& state) {
    auto result=region_snapshot(state);
    std::erase_if(result.items,[&](Region& region) {
        const auto& instance=*find_instance(state,region.id);
        if(!evaluate_visibility(state,instance,state.viewport.time))return true;
        const auto model=mesh_transform(ViewMode::scene,evaluate_transform(state,instance,state.viewport.time));
        for(auto& p:region.points) {
            const auto source=p;
            for(unsigned a=0;a<3;++a)p[a]=model[3][a]+model[0][a]*source.x+model[1][a]*source.y+model[2][a]*source.z;
        }
        return false;
    });
    return result;
}
std::optional<Region> region_world_snapshot(const State& state, u32 id) {
    const auto* instance = find_instance(state, id);
    if (!instance || !std::holds_alternative<RegionSettings>(instance->settings)) return {};
    const auto& settings = std::get<RegionSettings>(instance->settings);
    if (!evaluate_visibility(state, *instance, state.viewport.time)) return {};
    Region region;
    static_cast<RegionGeometry&>(region) = settings.boundary;
    region.id = id; region.name = instance->name; region.note = settings.note;
    region.show_walls = settings.show_walls;
    return region_to_world(state, region);
}
Region region_to_world(const State& state, const Region& local) {
    auto result = local;
    const auto* instance = find_instance(state, local.id); if (!instance) return result;
    const auto model = mesh_transform(ViewMode::scene, evaluate_transform(state, *instance, state.viewport.time));
    for (auto& p : result.points) {
        const auto source = p;
        for (unsigned a = 0; a < 3; ++a)
            p[a] = model[3][a] + model[0][a] * source.x + model[1][a] * source.y + model[2][a] * source.z;
    }
    return result;
}
Region region_to_local(const State& state,const Region& world) {
    auto result=world;
    const auto* instance=find_instance(state,world.id);if(!instance)return result;
    const auto transform=evaluate_transform(state,*instance,state.viewport.time);
    const auto model=mesh_transform(ViewMode::scene,transform);
    for(auto& p:result.points) {
        const auto source=p;
        for(unsigned a=0;a<3;++a) {
            double value{};
            for(unsigned c=0;c<3;++c)value+=(double(source[c])-model[3][c])*model[a][c];
            const auto scale=double(transform.scale)*transform.axis_scale[a];
            p[a]=static_cast<float>(value/(scale*scale));
        }
    }
    return result;
}
} // namespace editor_example
