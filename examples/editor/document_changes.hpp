#pragma once
#include <vng/timeline/timeline.hpp>
#include <vng/editor/mesh_patch.hpp>
#include <map>
#include <set>

namespace editor_example {
struct TargetLess {
    bool operator()(const vng::timeline::Target& a, const vng::timeline::Target& b) const {
        return a.object != b.object ? a.object < b.object : a.property < b.property;
    }
};
struct RegionChanges {
    bool whole{};
    std::set<vng::u32> points{};
    void merge(const RegionChanges& other) {
        if (whole || other.whole) { whole = true; points.clear(); }
        else points.insert(other.points.begin(), other.points.end());
    }
    friend bool operator==(const RegionChanges&, const RegionChanges&) = default;
};
// Authored change identity survives coalescing and history. No viewport state,
// GPU objects, before/after document comparisons or serialized payloads here.
struct DocumentChanges {
    bool full{}, duration{}, world_bounds{};
    std::map<vng::u32, RegionChanges> regions{}; // instance -> boundary or exact local point IDs
    std::map<vng::u32, std::set<vng::u32>> vertices{}; // blueprint -> exact vertex IDs
    std::set<vng::timeline::Target, TargetLess> properties{};
    std::set<vng::f32> markers{};
    std::map<vng::u32,vng::editor::MeshChanges> meshes{};
    std::set<vng::u32> mesh_placements{};
    [[nodiscard]] bool empty() const {
        return !full && !duration && !world_bounds && regions.empty() && vertices.empty() && properties.empty() && markers.empty() && meshes.empty() && mesh_placements.empty();
    }
    void merge(const DocumentChanges& other) {
        if (full || other.full) { *this = {.full = true}; return; }
        duration |= other.duration;
        world_bounds |= other.world_bounds;
        mesh_placements.insert(other.mesh_placements.begin(), other.mesh_placements.end());
        for (const auto& [instance, changes] : other.regions) regions[instance].merge(changes);
        properties.insert(other.properties.begin(), other.properties.end());
        markers.insert(other.markers.begin(), other.markers.end());
        for (const auto& [blueprint, ids] : other.vertices)
            vertices[blueprint].insert(ids.begin(), ids.end());
        for(const auto& [id,scope]:other.meshes)meshes[id].merge(scope);
        for(auto& [id,scope]:meshes)if(auto it=vertices.find(id);it!=vertices.end()) {
            if(!scope.whole)scope.fields["position"].insert(it->second.begin(),it->second.end());
            vertices.erase(it);
        }
    }
};
} // namespace editor_example
