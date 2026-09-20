#include "keyframe_inspector.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace editor_example {
namespace {
using namespace vng;
template<class T> std::string format(T value) {
    std::array<char, 64> bytes{};
    const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
    return result.ec == std::errc{} ? std::string(bytes.data(), result.ptr) : std::string{};
}
std::optional<f32> number(std::string_view text) {
    f32 value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return {};
    return value;
}
bool continuous(const timeline::Value& v) {
    return std::holds_alternative<f32>(v) || std::holds_alternative<Vec3>(v);
}
}
struct KeyframeInspector::Impl {
    struct Field {
        AnimationProperty property;
        ui::Container row;
        ui::Checkbox keyed, blend, boolean;
        std::array<ui::TextField, 3> values;
        timeline::Value sampled;
        bool vector{}, logical{};
    };
    ui::Container panel, controls, form;
    ui::Label title, status;
    ui::TextField time;
    ui::Button apply, revert, move, erase;
    std::vector<Field> fields;
    std::optional<f32> selected;
    bool dirty{};
    explicit Impl(ui::Container p) : panel(p) {
        panel.padding(10).gap(6);
        title = panel.label("KEYFRAME / none selected").height(30);
        panel.label("Select a marker, or point at a time and Add keyframe.").height(28);
        controls = panel.column().padding(0).gap(6);
        auto position = controls.row().height(36).padding(0).gap(6);
        position.label("Time (s)").width(88);
        time = position.text_input().width(100);
        move = position.button("Move").width(82);
        erase = position.button("Delete keyframe").width(172);
        auto actions = controls.row().height(36).padding(0).gap(6);
        apply = actions.button("Apply keyframe").width(176);
        revert = actions.button("Reset changes").width(166);
        controls.label("Key = explicit value. Blend = interpolate from previous.").height(28);
        form = controls.column().padding(0).gap(6);
        status = panel.label("No keyframe selected.").height(48);
        controls.visible(false);
    }
    void build(std::span<const AnimationProperty> properties) {
        form.remove();
        form = controls.column().padding(0).gap(6);
        fields.clear();
        u64 object{};
        for (const auto& property : properties) {
            if (property.target.object != object) {
                object = property.target.object;
                form.label(property.layer).height(30);
            }
            Field field;
            field.property = property;
            field.vector = std::holds_alternative<Vec3>(property.base_value);
            field.logical = std::holds_alternative<bool>(property.base_value);
            field.row = form.column().height(field.vector ? 78.F : 38.F).padding(0).gap(4);
            auto row = field.row.row().height(36).padding(0).gap(6);
            row.label(property.label).width(124);
            if (!field.vector) {
                if (field.logical) field.boolean = row.checkbox("On").width(98);
                else field.values[0] = row.text_input().width(98);
            }
            field.keyed = row.checkbox("Key").width(72);
            field.blend = row.checkbox("Blend").width(98);
            if (field.vector) {
                auto xyz = field.row.row().height(36).padding(0).gap(6);
                for (std::size_t i = 0; i < 3; ++i) {
                    xyz.label(i == 0 ? "X" : i == 1 ? "Y" : "Z").width(28);
                    field.values[i] = xyz.text_input().width(110);
                }
            }
            fields.push_back(std::move(field));
        }
    }
    bool editing() const {
        if (time.isFocused()) return true;
        for (const auto& field : fields) {
            if (field.keyed.isFocused() || field.blend.isFocused()) return true;
            if (field.logical) { if (field.boolean.isFocused()) return true; }
            else for (std::size_t i = 0; i < (field.vector ? 3U : 1U); ++i)
                if (field.values[i].isFocused()) return true;
        }
        return false;
    }
    void enabled(Field& field) {
        const auto keyed = field.keyed.value();
        field.blend.enabled(keyed && continuous(field.sampled));
        if (field.logical) field.boolean.enabled(keyed);
        else for (std::size_t i = 0; i < (field.vector ? 3U : 1U); ++i) field.values[i].enabled(keyed);
    }
};
KeyframeInspector::KeyframeInspector(ui::Container panel) : impl_(std::make_unique<Impl>(panel)) {}
KeyframeInspector::~KeyframeInspector() = default;
void KeyframeInspector::show(std::span<const AnimationProperty> properties,
                             const timeline::Timeline& timeline, std::optional<f32> time, bool reset) {
    auto& p = *impl_;
    bool schema = properties.size() != p.fields.size();
    if (!schema) for (std::size_t i = 0; i < properties.size(); ++i)
        schema |= p.fields[i].property.target != properties[i].target || p.fields[i].property.label != properties[i].label;
    if (schema) p.build(properties);
    const bool changed = p.selected != time || reset || schema;
    p.selected = time;
    p.controls.visible(time.has_value());
    p.title.text(time ? "KEYFRAME / " + format(*time) + " s" : "KEYFRAME / none selected");
    if (!time) { p.dirty = false; p.status.text("Click the timeline, then Add keyframe."); return; }
    if (!changed && (p.dirty || p.editing())) return;
    p.time.value(format(*time));
    std::size_t count{};
    for (std::size_t i = 0; i < p.fields.size(); ++i) {
        auto& field = p.fields[i];
        field.sampled = timeline.sample(field.property.target, *time).value_or(properties[i].base_value);
        const timeline::Keyframe* key{};
        if (const auto* track = timeline.find(field.property.target)) {
            auto found = std::ranges::find(track->keys, *time, &timeline::Keyframe::time);
            if (found != track->keys.end()) key = &*found;
        }
        field.keyed.value(key != nullptr);
        field.blend.value(key && key->incoming == timeline::Interpolation::linear);
        count += key != nullptr;
        if (field.logical) field.boolean.value(std::get<bool>(field.sampled));
        else if (field.vector) for (std::size_t c = 0; c < 3; ++c)
            field.values[c].value(format(std::get<Vec3>(field.sampled)[c]));
        else field.values[0].value(format(std::get<f32>(field.sampled)));
        p.enabled(field);
    }
    p.dirty = false;
    p.status.text(std::to_string(count) + " explicit values. Unkeyed rows show evaluated values.\n"
                  "Apply edits this timestamp only; base settings stay unchanged.");
}
std::optional<TimelineAction> KeyframeInspector::poll() {
    auto& p = *impl_;
    if (!p.selected) return {};
    p.dirty |= p.time.changedText().has_value();
    for (auto& field : p.fields) {
        p.dirty |= field.keyed.changedValue().has_value() || field.blend.changedValue().has_value();
        if (field.logical) p.dirty |= field.boolean.changedValue().has_value();
        else for (std::size_t i = 0; i < (field.vector ? 3U : 1U); ++i)
            p.dirty |= field.values[i].changedText().has_value();
        if (field.keyed.changedValue() == true && continuous(field.sampled)) field.blend.value(true);
        p.enabled(field);
    }
    if (p.revert.clicked()) { p.dirty = false; p.selected.reset(); return {}; }
    if (p.erase.clicked()) return TimelineAction{.kind = TimelineAction::Kind::erase, .time = *p.selected};
    if (p.move.clicked()) {
        const auto to = number(p.time.getText());
        if (!to) { p.status.text("Enter a finite keyframe time."); return {}; }
        return TimelineAction{.kind = TimelineAction::Kind::move, .time = *p.selected, .destination = *to};
    }
    if (!p.apply.clicked()) return {};
    TimelineAction action{.kind = TimelineAction::Kind::edit, .time = *p.selected};
    for (const auto& field : p.fields) {
        auto value = field.sampled;
        if (field.keyed.value()) {
            if (field.logical) value = field.boolean.value();
            else {
                Vec3 vector{};
                for (std::size_t i = 0; i < (field.vector ? 3U : 1U); ++i) {
                    const auto parsed = number(field.values[i].getText());
                    if (!parsed) { p.status.text("Enter finite numbers for " + field.property.label + "."); return {}; }
                    vector[i] = *parsed;
                }
                value = field.vector ? timeline::Value{vector} : timeline::Value{vector.x};
            }
        }
        action.values.push_back({field.property.target, value,
            continuous(value) && field.blend.value() ? timeline::Interpolation::linear : timeline::Interpolation::hold,
            field.keyed.value()});
    }
    return action;
}
void KeyframeInspector::error(std::string_view message) { impl_->status.text(message); impl_->dirty = true; }
} // namespace editor_example
