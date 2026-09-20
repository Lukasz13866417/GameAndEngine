#include "transform_pivot.hpp"
#include "animation.hpp"
#include "rotation_math.hpp"
#include "../support/mesh_frame.hpp"
#include <algorithm>

namespace editor_example {
using namespace vng;
std::vector<InstanceCenter> instance_centers(const State& state,u32 primary,std::span<const u32> selection) {
    std::vector<u32> ids{primary};
    for(auto id:selection)if(std::ranges::find(ids,id)==ids.end())ids.push_back(id);
    std::map<BlueprintId,Vec3> mesh_centers;
    std::vector<InstanceCenter> result;
    for(auto id:ids)if(const auto* instance=find_instance(state,id)) {
        auto transform=evaluate_transform(state,*instance,state.viewport.time);
        Vec3 local{};
        if(const auto* region=std::get_if<RegionSettings>(&instance->settings)) {
            for(auto p:region->boundary.points)for(unsigned c=0;c<3;++c)local[c]+=p[c]/static_cast<float>(region->boundary.points.size());
        } else if(auto* mesh=mesh_view_geometry(state,instance->blueprint)) {
            auto [it,inserted]=mesh_centers.try_emplace(instance->blueprint);
            if(inserted && mesh->size()) {
                std::array<double,3> sum{};
                for(u32 v=0;v<mesh->size();++v)for(unsigned c=0;c<3;++c)sum[c]+=mesh->position(v)[c];
                for(unsigned c=0;c<3;++c)it->second[c]=static_cast<float>(sum[c]/static_cast<double>(mesh->size()));
            }
            local=example::mesh_frame::point(mesh_placement(state,instance->blueprint,
                state.viewport.mode==ViewMode::mesh),it->second);
        }
        auto scaled=local;for(unsigned c=0;c<3;++c)scaled[c]*=transform.axis_scale[c];
        auto offset=rotation_math::direction(transform.rotation,scaled);
        Vec3 world{};for(unsigned c=0;c<3;++c)world[c]=transform.position[c]+offset[c]*transform.scale;
        result.push_back({id,transform,local,world});
    }
    return result;
}
Vec3 selection_center(std::span<const InstanceCenter> centers) {
    std::array<double,3> sum{};Vec3 result{};
    for(const auto& center:centers)for(unsigned c=0;c<3;++c)sum[c]+=center.world[c];
    if(!centers.empty())for(unsigned c=0;c<3;++c)result[c]=static_cast<float>(sum[c]/static_cast<double>(centers.size()));
    return result;
}
}
