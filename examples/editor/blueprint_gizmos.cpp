#include "blueprint_gizmos.hpp"
#include "rotation_math.hpp"

#include <array>
#include <algorithm>
#include <unordered_map>

namespace editor_example {
namespace {
std::optional<vng::Vec3> direction(std::string_view name) {
    constexpr std::array<std::string_view,6> names{"+X","-X","+Y","-Y","+Z","-Z"};
    constexpr std::array<vng::Vec3,6> axes{{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}};
    for (std::size_t i=0;i<names.size();++i) if(name==names[i])return axes[i];
    return {};
}
}
BlueprintManipulation blueprint_manipulation(const State& state, BlueprintId id) {
    BlueprintManipulation result;
    if (id == BlueprintId::region) {
        result.surface = ManipulationSurface::boundary;
        result.gizmos.insert(result.gizmos.end(),
            {GizmoMode::region_vertices, GizmoMode::region_edges, GizmoMode::region_faces});
        return result;
    }
    const auto* mesh = mesh_geometry(state, id);
    if (!mesh) return result;
    const auto& info = mesh->document().metadata;
    const auto axis = [&](std::string_view name) -> std::optional<vng::Vec3> {
        const auto found = info.find(std::string{name});
        return found == info.end() ? std::nullopt : direction(found->second);
    };
    const auto enabled = [&](std::string_view name) {
        const auto found = info.find(std::string{name});
        return found == info.end() || found->second != "off";
    };
    const auto forward = axis("coordinates/forward"), up = axis("coordinates/up");
    if (forward && enabled("editor/gizmos/forward")) {
        result.forward = forward;
        result.gizmos.push_back(GizmoMode::forward);
    }
    if (forward && up && enabled("editor/gizmos/attitude") &&
        vng::gfx::camera_detail::dot(*forward, *up) == 0) {
        result.attitude = std::array{*up, vng::gfx::camera_detail::cross(*forward, *up), *forward};
        result.gizmos.push_back(GizmoMode::attitude);
    }
    return result;
}
std::optional<std::array<vng::Vec3,3>> blueprint_attitude_axes(const State& state, BlueprintId id) {
    return blueprint_manipulation(state, id).attitude;
}
std::vector<GizmoMode> selection_gizmos(const State& state, std::span<const vng::u32> selection) {
    if(state.viewport.mode!=ViewMode::scene || selection.empty())return {};
    std::unordered_map<vng::u32,const SceneInstance*> instances;
    for(const auto& instance:scene_instances(state))instances.emplace(instance.id,&instance);
    std::unordered_map<BlueprintId, BlueprintManipulation> capabilities;
    std::optional<std::vector<GizmoMode>> common;
    for(auto id:selection) {
        const auto found=instances.find(id); if(found==instances.end())return {};
        const auto* instance=found->second;
        auto at=capabilities.find(instance->blueprint);
        if(at==capabilities.end())at=capabilities.emplace(instance->blueprint,
            blueprint_manipulation(state, instance->blueprint)).first;
        if (!common) common = at->second.gizmos;
        else std::erase_if(*common, [&](GizmoMode mode) {
            return std::ranges::find(at->second.gizmos, mode) == at->second.gizmos.end();
        });
    }
    return std::move(*common);
}
void filter_translation_gizmos(vng::editor::Schema& schema, std::span<const GizmoMode> common) {
    if(std::ranges::find(common,GizmoMode::forward)!=common.end())return;
    for(auto& control:schema.controls) if(control.key=="position") control.translation_axes.clear();
}
std::vector<vng::editor::TranslationAxis>
blueprint_translation_axes(const State& state, const SceneInstance& evaluated) {
    using namespace vng;
    if(auto axis=blueprint_manipulation(state, evaluated.blueprint).forward)
        return {{"Forward / back",rotation_math::direction(evaluated.transform.rotation,*axis)}};
    return {}; // Unknown optional orientation does not invent a movement axis.
}
}
