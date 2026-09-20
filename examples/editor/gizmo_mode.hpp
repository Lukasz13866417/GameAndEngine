#pragma once
#include <vng/input/input.hpp>
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace editor_example {
enum class GizmoMode { move, rotate, scale, forward, attitude, region_vertices, region_edges, region_faces, free_rotate };
enum class TransformKind { move, rotate, scale };
struct GizmoDescription {
    GizmoMode mode;
    std::string_view label;
    vng::input::Key key;
    std::string_view shortcut;
    // A null gesture selects the gizmo; its handles own the interaction.
    std::optional<TransformKind> gesture;
};
inline constexpr std::array gizmo_descriptions{
    GizmoDescription{GizmoMode::move,"Move",vng::input::Key::g,"G",TransformKind::move},
    GizmoDescription{GizmoMode::rotate,"Rotate",vng::input::Key::r,"R",TransformKind::rotate},
    GizmoDescription{GizmoMode::free_rotate,"Free rotate",vng::input::Key::unknown,"MMB drag",{}},
    GizmoDescription{GizmoMode::scale,"Scale",vng::input::Key::s,"S",TransformKind::scale},
    GizmoDescription{GizmoMode::forward,"Forward / back",vng::input::Key::f,"F",TransformKind::move},
    GizmoDescription{GizmoMode::attitude,"Yaw/pitch/roll",vng::input::Key::t,"T",{}},
    GizmoDescription{GizmoMode::region_vertices,"Region vertices",vng::input::Key::one,"1",{}},
    GizmoDescription{GizmoMode::region_edges,"Region edges",vng::input::Key::two,"2",{}},
    GizmoDescription{GizmoMode::region_faces,"Region faces",vng::input::Key::three,"3",{}}
};
inline constexpr std::array basic_transform_gizmos{GizmoMode::move,GizmoMode::rotate,GizmoMode::scale,GizmoMode::free_rotate};
inline TransformKind gizmo_transform_kind(GizmoMode mode) {
    return mode==GizmoMode::scale ? TransformKind::scale :
        mode==GizmoMode::rotate || mode==GizmoMode::free_rotate ? TransformKind::rotate : TransformKind::move;
}
inline GizmoMode transform_gizmo(TransformKind kind) {
    return kind==TransformKind::scale ? GizmoMode::scale : kind==TransformKind::rotate ? GizmoMode::rotate : GizmoMode::move;
}
inline const GizmoDescription& gizmo_description(GizmoMode mode) {
    return *std::ranges::find(gizmo_descriptions,mode,&GizmoDescription::mode);
}
inline std::string_view gizmo_label(GizmoMode mode) { return gizmo_description(mode).label; }
inline std::string gizmo_choice_label(GizmoMode mode) {
    const auto& description=gizmo_description(mode);
    return std::string(description.label)+" ("+std::string(description.shortcut)+")";
}
inline bool region_component_mode(GizmoMode mode) {
    return mode==GizmoMode::region_vertices || mode==GizmoMode::region_edges || mode==GizmoMode::region_faces;
}
inline bool translation_mode(GizmoMode mode) { return mode==GizmoMode::move || mode==GizmoMode::forward; }
inline bool rotation_mode(GizmoMode mode) { return mode==GizmoMode::rotate || mode==GizmoMode::attitude || mode==GizmoMode::free_rotate; }
// The same descriptions drive both menu hints and input. Only the supplied
// capabilities are eligible; callers supply unhandled viewport events.
inline std::optional<GizmoMode> gizmo_shortcut(const vng::input::Event& event,
    std::span<const GizmoMode> available) {
    using namespace vng;
    if(event.kind!=input::EventKind::key_down || event.repeat || event.modifiers.control ||
       event.modifiers.alt || event.modifiers.super || event.modifiers.shift) return {};
    for(auto mode:available) if(event.key!=input::Key::unknown && gizmo_description(mode).key==event.key) return mode;
    return {};
}
}
