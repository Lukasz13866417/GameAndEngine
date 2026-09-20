#pragma once
#include <vng/ui/draw_list.hpp>
#include <algorithm>
#include <cmath>

namespace editor_example {
// Logical pixels: resizing/maximizing changes panel space, never input scale.
struct EditorLayout {
    vng::ui::Rect toolbar, scene, keyframes, tabs, inspector, caption, viewport;
    vng::ui::Rect timeline, files, status, logs, dialog;
};
inline vng::f32 editor_list_height(const EditorLayout& layout) {
    // Camera controls live in their own flyout. Both lists scroll independently.
    return std::clamp((layout.scene.height - 200.0F) * .5F, 60.0F, 208.0F);
}
// A zero aspect means an editable viewport: render at the available dimensions.
// Independent playback may request its fixed output aspect instead.
inline EditorLayout editor_layout(vng::Vec2 size, bool vertices, vng::f32 preview_aspect = 0) {
    using namespace vng;
    const f32 width = std::max(800.0F, size.x), height = std::max(500.0F, size.y);
    const f32 margin = 12;
    f32 right = std::clamp(width*.24F,480.F,600.F);
    const f32 body_bottom = height - (height<760 ? 200.F : 264.F), body_height = body_bottom - 126;
    const f32 available_width = width - right - margin - 32;
    const f32 spare = std::isfinite(preview_aspect) && preview_aspect > 0
        ? std::max(0.F, available_width - (body_height - 60) * preview_aspect) : 0.F;
    right += spare;
    const f32 center_x = 16, center_width = available_width - spare;
    EditorLayout result;
    result.toolbar = {16, 12, width - 32, 100};
    result.tabs = {width - right - 16, 126, right, 38};
    result.inspector = {result.tabs.x, 170, right, body_height - 44};
    const auto keys_height = std::clamp(result.inspector.height * (vertices ? .14F : .18F), 70.F, 160.F);
    const auto scene_height = std::max(60.F, result.inspector.height - keys_height - margin);
    result.scene = {result.inspector.x, result.inspector.y, right, scene_height};
    result.keyframes = {result.scene.x, result.scene.y + scene_height + margin, right,
                       std::max(30.F,result.inspector.height - scene_height - margin)};
    result.caption = {center_x, 126, center_width, 54};
    result.viewport = {center_x, 186, center_width, body_height - 60};
    result.timeline = {16, body_bottom + margin, width - 32, height<760 ? 96.F : 160.F};
    result.files = {16, height - 80, width - 32, 44};
    result.status = {16, height - 32, width - 32, 28};
    result.logs = result.viewport;
    result.dialog = {std::max(0.0F, (width - 620) * .5F), std::max(0.0F, (height - 230) * .5F), 620,
                     230};
    return result;
}
// Detached viewport: the same tabbed sidebar fills the controls window.
// Keep the timeline compact, with its full width and controls.
inline EditorLayout editor_controls_layout(vng::Vec2 size, bool vertices) {
    using namespace vng;
    auto result = editor_layout(size, vertices);
    const auto width=std::max(800.F,size.x), height=std::max(500.F,size.y);
    const auto body_bottom=height-200.F, body_height=body_bottom-126.F;
    result.tabs={16,126,width-32,38};
    result.inspector={16,170,width-32,body_height-44};
    const auto scene_height=std::max(60.F,result.inspector.height-std::clamp(result.inspector.height*.18F,70.F,120.F)-12.F);
    result.scene={16,170,width-32,scene_height};
    result.keyframes={16,182+scene_height,width-32,std::max(30.F,result.inspector.height-scene_height-12)};
    result.timeline={16,body_bottom+12,width-32,96};
    result.viewport={}; result.caption={};
    result.logs={16,126,width-32,body_height};
    return result;
}
} // namespace editor_example
