#pragma once
#include "editing_session.hpp"
#include <vng/editor/inspector.hpp>
#include <algorithm>

namespace editor_example {
// Inspector adapter for portable instance placement. Other controls are native
// project requests; only the worker runs their hot-reloaded implementation.
inline vng::content::Result<std::optional<bool>> apply_instance_control(
    EditingSession& session, const vng::editor::Event& event, vng::u64 generation,
    vng::u64 minimum_context) {
    using namespace vng;
    const auto& state = session.state();
    if (event.control != "transform" || state.viewport.mode != ViewMode::scene) return std::optional<bool>{};
    const auto invalid = [](std::string message) {
        content::Diagnostic error; error.message = std::move(message);
        return std::unexpected(std::move(error));
    };
    if (event.stamp.generation != generation || event.stamp.revision != state.document.revision ||
        event.stamp.context < minimum_context || event.stamp.object != state.viewport.selected_object ||
        !event.stamp.object || event.stamp.object > UINT32_MAX || event.phase != editor::Phase::apply)
        return invalid("Stale instance transform control; wait for current controls");
    const auto scale = std::ranges::find(event.values, std::string{"scale"}, &editor::NamedValue::key);
    const auto rotation = std::ranges::find(event.values, std::string{"rotation"}, &editor::NamedValue::key);
    const auto axes = std::ranges::find(event.values, std::string{"axis_scale"}, &editor::NamedValue::key);
    if ((event.values.size() != 2 && event.values.size()!=3) || scale == event.values.end() || rotation == event.values.end() ||
        !std::holds_alternative<f32>(scale->value) || !std::holds_alternative<Vec3>(rotation->value) ||
        (event.values.size()==3 && (axes==event.values.end() || !std::holds_alternative<Vec3>(axes->value))))
        return invalid("Instance transform requires scale, rotation and optional local axis scale");
    auto edited = session.set_transform(static_cast<u32>(event.stamp.object),
        std::get<Vec3>(rotation->value), std::get<f32>(scale->value),
        axes==event.values.end() ? std::optional<Vec3>{} : std::get<Vec3>(axes->value));
    if (!edited) return std::unexpected(edited.error());
    return std::optional{*edited};
}
}
