#include "timeline_panel.hpp"
#include "selection_input.hpp"
#include "keyframe_range_menu.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <ranges>
#include <set>

namespace editor_example {
namespace {
using namespace vng;
std::string format(f32 value) {
    std::array<char, 64> buffer{};
    const auto [end, code] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return code == std::errc{} ? std::string(buffer.data(), end) : std::string{};
}
std::optional<f32> number(std::string_view text) {
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    f32 value{};
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || code != std::errc{} || end != text.data() + text.size() ||
        !std::isfinite(value))
        return {};
    return value;
}
bool continuous(const timeline::Value& value) {
    return std::holds_alternative<f32>(value) || std::holds_alternative<Vec3>(value);
}
bool finite(Vec2 value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}
} // namespace

struct TimelinePanel::Impl {
    struct Row {
        AnimationProperty property;
        ui::Container host, value_host;
        ui::Label label;
        ui::Checkbox keyed, blend, boolean;
        std::array<ui::TextField, 3> fields;
        KeyframeValue value;
        bool differs{}, draft{};
    };
    struct Group {
        u64 object{};
        ui::Container host, details;
        ui::Button toggle, identifier;
        std::vector<Row> rows;
        std::size_t row_count{};
    };
    ui::Container strip, list, inspector, list_items, controls, property_rows, layer_host, object_host;
    ui::Container bar, metadata, timestamp;
    ui::Label title, hint, status, selection_count;
    ui::TextField time, duration, name, key_time;
    ui::Slider playhead;
    ui::Button add, apply, apply_range, erase;
    ui::Container action_host;
    KeyframeRangeMenu range_menu;
    std::optional<ui::Dropdown<std::string>> layer;
    std::optional<ui::Dropdown<u64>> object;
    ui::ImageView canvas;
    std::shared_ptr<const gfx::ImageData> anchor;
    std::vector<Group> groups;
    // Selection supplies the default. Explicit toggles last until that object's
    // selection changes; expansion is UI state, never an authored property.
    std::map<u64, bool> expansion;
    std::vector<std::pair<f32, ui::Button>> entries;
    std::vector<f32> times, visible_times;
    std::vector<AnimationProperty> properties;
    std::set<u64> selected_objects;
    // A panel never owns a document/mesh snapshot. Only the small data it
    // actually inspects crosses this boundary.
    struct Snapshot {
        u64 revision{};
        f32 duration{}, time{};
        bool paused{true};
        struct ObjectInfo {
            std::string name, identifier;
            friend bool operator==(const ObjectInfo&, const ObjectInfo&) = default;
        };
        // Instance name for the filter, blueprint + instance for each heading.
        std::map<u64, ObjectInfo> objects;
        timeline::Timeline animation;
        std::map<f32, std::string> names;
        std::string key_name(f32 time_) const {
            const auto found = names.find(time_);
            return found == names.end() ? std::string{} : found->second;
        }
        std::string object_name(u64 id) const {
            const auto found = objects.find(id);
            return found == objects.end() ? std::string{} : found->second.identifier;
        }
    } snapshot;
    // Reuse the high-water row pool on seeks, filtering and schema changes.
    // Removing/recreating retained widgets also used up their stable IDs.
    std::size_t group_count{};
    std::span<Group> active_groups() { return {groups.data(), group_count}; }
    std::span<const Group> active_groups() const { return {groups.data(), group_count}; }
    auto active_rows() {
        return active_groups() | std::views::transform([](Group& group) {
            return std::span<Row>{group.rows.data(), group.row_count};
        }) | std::views::join;
    }
    auto active_rows() const {
        return active_groups() | std::views::transform([](const Group& group) {
            return std::span<const Row>{group.rows.data(), group.row_count};
        }) | std::views::join;
    }
    bool expanded(u64 id) const {
        if (const auto found = expansion.find(id); found != expansion.end()) return found->second;
        return selected_objects.contains(id);
    }
    std::optional<f32> selected, awaiting;
    editor::Selection<f32> selection;
    TimelinePanel::Statistics statistics;
    bool initialized{}, dirty{}, dragging{}, handled{}, refresh{true}, rebuild_list{true};
    bool rows_dirty{true};
    std::optional<f32> row_width;
    ui::Rect drag_bounds;

    Impl(ui::Container strip_, ui::Container list_, ui::Container inspector_, ui::Container actions,
         ui::Container menu)
        : strip(strip_), list(list_), inspector(inspector_), action_host(actions), range_menu(menu) {
        actions.padding(0).gap(6).height(36).width(440);
        apply = actions.button("Apply to keyframe").width(208).enabled(false);
        apply_range = actions.button("Apply to keyframes").width(226).enabled(false);
        strip.padding(8).gap(6);
        bar = strip.row().height(36).padding(0).gap(8);
        bar.label("TIMELINE").width(110);
        add = bar.button("Add keyframe").width(162);
        bar.label("Time").width(62);
        time = bar.text_input().width(92);
        bar.label("Duration").width(96);
        duration = bar.text_input().width(92);
        layer_host = bar.column().width(210).height(36).padding(0);
        playhead = bar.slider("Playhead", 0, 10);
        canvas = strip.image().height(72);
        anchor = std::make_shared<gfx::ImageData>(
            gfx::ImageData{{1, 1}, {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}}});
        canvas.image(anchor);
        status = strip.label("Click to select / Shift-click: range / Ctrl-click: toggle / Delete: selected keys")
                     .height(24);
        list.padding(8).gap(6);
        selection_count = list.label("KEYFRAMES / 0 selected").height(26);
        list_items = list.column().padding(0).gap(4);
        inspector.padding(10).gap(6);
        title = inspector.label("KEYFRAME / none selected").height(28);
        hint = inspector.label("Choose a marker or Add keyframe at the playhead.").height(24);
        controls = inspector.column().padding(0).gap(6).visible(false);
        metadata = controls.row().height(36).padding(0).gap(8);
        metadata.label("Name").width(70);
        name = metadata.text_input();
        timestamp = controls.row().height(36).padding(0).gap(8);
        timestamp.label("Timestamp (s)").width(156);
        key_time = timestamp.text_input().width(110);
        erase = controls.button("Delete keyframe").width(172);
        object_host = controls.column().height(36).padding(0);
        object = object_host.dropdown<u64>("Object", {{0, "All objects"}});
        controls.label("Key = stored here. Blend = interpolate FROM the previous value.")
            .height(24);
        property_rows = controls.column().padding(0).gap(6);
    }
    bool exists(f32 time_) const { return std::ranges::find(times, time_) != times.end(); }
    bool editing() const {
        if (name.isFocused() || key_time.isFocused())
            return true;
        return std::ranges::any_of(active_rows(), [](const Row& row) {
            return row.keyed.isFocused() || row.blend.isFocused() || row.boolean.isFocused() ||
                   std::ranges::any_of(row.fields,
                                       [](const auto& field) { return field.isFocused(); });
        });
    }
    void layout() {
        // Bounds of the actual content rows already exclude every ancestor's
        // padding and scrollbar gutter. Outer panel widths do not.
        playhead.width(std::max(0.0F, bar.bounds().width - 880));
        list_items.height(std::max(40.0F, list.bounds().height - 48));
        name.width(std::max(0.0F, metadata.bounds().width - 78));
        key_time.width(std::max(0.0F, timestamp.bounds().width - 164));
        const auto content_width = property_rows.bounds().width;
        const auto value_width = std::max(0.0F, content_width - 340);
        if (row_width == content_width) return;
        row_width = content_width;
        ++statistics.row_layout_updates;
        for (auto& group : active_groups())
            group.identifier.width(std::max(1.F, content_width - 34));
        for (auto& row : active_rows()) {
            row.value_host.width(value_width);
            row.boolean.width(value_width);
            const auto field_width = std::holds_alternative<Vec3>(row.value.value)
                ? std::max(0.0F, (value_width - 8) / 3) : value_width;
            for (auto& field : row.fields) field.width(field_width);
        }
    }
    void refresh_filters() {
        const auto previous_object = object ? object->value() : 0;
        std::vector<ui::Choice<u64>> objects{{0, "All objects"}};
        for (const auto& [id, names] : snapshot.objects)
            objects.push_back({id, names.name});
        if (object) object->remove();
        object.emplace(object_host.dropdown<u64>("Object", objects));
        if (std::ranges::any_of(objects, [&](const auto& item) { return item.value == previous_object; }))
            object->value(previous_object);
        const auto previous = layer ? layer->value() : std::string{};
        std::vector<ui::Choice<std::string>> choices{{"", "All layers"}};
        for (const auto& property : properties)
            if (std::ranges::none_of(
                    choices, [&](const auto& choice) { return choice.value == property.layer; }))
                choices.push_back({property.layer, property.layer});
        if (layer)
            layer->remove();
        layer.emplace(layer_host.dropdown<std::string>("Layer", choices));
        layer->width(210);
        if (std::ranges::any_of(choices,
                                [&](const auto& choice) { return choice.value == previous; }))
            layer->value(previous);
        rebuild_list = true;
    }
    void update_list() {
        if (!rebuild_list)
            return;
        selection_count.text("KEYFRAMES / " + std::to_string(selection.size()) + " selected");
        std::vector<f32> filtered_times;
        filtered_times.reserve(times.size());
        const auto filter = layer ? layer->value() : std::string{};
        for (auto time_ : times) {
            bool visible = time_ == 0 || filter.empty();
            if (!visible)
                for (const auto& property : properties)
                    if (property.layer == filter)
                        if (const auto* track = snapshot.animation.find(property.target))
                            visible |=
                                std::ranges::find(track->keys, time_, &timeline::Keyframe::time) !=
                                track->keys.end();
            if (!visible)
                continue;
            filtered_times.push_back(time_);
        }
        // Object drags and selection changes also increment the scene revision.
        // Keep the existing buttons unless the visible timestamp set changes;
        // rebuilding them each time wastes layout work and exhausts widget IDs.
        if (visible_times != filtered_times) {
            for (auto& [time_, button] : entries) {
                (void)time_;
                button.remove();
            }
            entries.clear();
            visible_times = std::move(filtered_times);
            for (const auto time_ : visible_times)
                entries.emplace_back(time_, list_items.button("").height(34));
        }
        for (auto& [time_, button] : entries) {
            const auto custom_name = snapshot.key_name(time_);
            button.text((selected == time_ ? "> " : selection.contains(time_) ? "+ " : "") + format(time_) + " s | " +
                        (custom_name.empty() ? "Keyframe" : custom_name));
        }
        rebuild_list = false;
    }
    void update_rows() {
        if (!rows_dirty) return;
        rows_dirty = false;
        ++statistics.row_visibility_updates;
        std::set<u64> shown(selected_objects.begin(), selected_objects.end());
        for (const auto& row : active_rows())
            if (row.differs || row.draft) shown.insert(row.property.target.object);
        for (auto& group : active_groups()) {
            group.host.visible(shown.contains(group.object) &&
                               (!object->value() || object->value() == group.object));
            const bool open = expanded(group.object);
            group.toggle.text(open ? "-" : "+");
            group.identifier.selected(selected_objects.contains(group.object));
            group.details.visible(open);
        }
        for (auto& row : active_rows()) {
            const bool keyed = row.keyed.value();
            row.keyed.enabled(selected != 0.F);
            row.value_host.enabled(keyed);
            row.blend.enabled(selected != 0.F && keyed && continuous(row.value.value));
        }
    }
    std::optional<KeyframeValue> read(const Row& row) const {
        auto field = row.value;
        field.keyed = row.keyed.value();
        field.incoming = row.blend.value() && continuous(field.value)
            ? timeline::Interpolation::linear : timeline::Interpolation::hold;
        if (field.keyed) {
            if (std::holds_alternative<Vec3>(field.value)) {
                Vec3 value{};
                for (std::size_t n=0; n<3; ++n) {
                    const auto parsed = number(row.fields[n].getText());
                    if (!parsed) return {};
                    value[n] = *parsed;
                }
                field.value = value;
            } else if (std::holds_alternative<f32>(field.value)) {
                const auto value = number(row.fields[0].getText());
                if (!value) return {};
                field.value = *value;
            } else if (std::holds_alternative<bool>(field.value)) field.value = row.boolean.value();
        }
        return field;
    }
    void inspect() {
        ++statistics.value_refreshes;
        controls.visible(selected.has_value());
        controls.enabled(selected && snapshot.paused && snapshot.time == *selected);
        apply.enabled(selected && snapshot.paused && snapshot.time == *selected);
        apply_range.enabled(selected && snapshot.paused && snapshot.time == *selected);
        if (!selected) {
            title.text("KEYFRAME / none selected");
            hint.text("Choose a marker or Add keyframe at the playhead.");
            group_count = 0;
            refresh = false;
            return;
        }
        title.text("KEYFRAME / " + format(*selected) + " seconds");
        hint.text(selection.size()>1 ? std::to_string(selection.size())+" selected / inspector edits active; Delete preserves zero"
                  : *selected == 0 ? "Initial pose / permanent keyframe / all instances"
                                   : "Changed instances + selection + unsaved edits");
        erase.text(selection.size()>1 ? "Delete selected keys" : "Delete keyframe");
        erase.width(selection.size()>1 ? 226.F : 172.F);
        erase.enabled(std::ranges::any_of(selection.items(), [](f32 t){ return t != 0; }));
        key_time.enabled(*selected != 0);
        name.value(snapshot.key_name(*selected));
        key_time.value(format(*selected));
        const auto values = keyframe_values(properties, snapshot.animation, *selected);
        const auto at = std::ranges::lower_bound(times, *selected);
        const auto previous = keyframe_values(properties, snapshot.animation,
                                              at == times.begin() ? 0.F : *std::prev(at));
        group_count = 0;
        for (std::size_t first = 0; first < properties.size();) {
            const auto id = properties[first].target.object;
            auto end = first + 1;
            while (end < properties.size() && properties[end].target.object == id) ++end;
            if (group_count == groups.size()) {
                Group group;
                group.host = property_rows.column().padding(0).gap(3);
                auto header = group.host.row().height(28).padding(0).gap(6);
                group.toggle = header.button("+").width(28).height(28);
                group.identifier = header.button("").height(28);
                group.details = group.host.column().padding(0).gap(3);
                groups.push_back(std::move(group));
            }
            auto& group = groups[group_count++];
            group.object = id;
            group.identifier.text(snapshot.object_name(id));
            group.row_count = end - first;
            while (group.rows.size() < group.row_count) {
                Row row;
                row.host = group.details.row().height(36).padding(0).gap(6);
                auto line = row.host;
                row.label = line.label("").width(150);
                row.keyed = line.checkbox("Key").width(72);
                row.value_host = line.row().width(218).height(36).padding(0).gap(4);
                for (auto& field : row.fields)
                    field = row.value_host.text_input().width(70).visible(false);
                row.boolean = row.value_host.checkbox("Enabled").width(210).visible(false);
                row.blend = line.checkbox("Blend").width(100);
                group.rows.push_back(std::move(row));
            }
            for (std::size_t i = group.row_count; i < group.rows.size(); ++i) group.rows[i].host.visible(false);
            for (std::size_t i = first; i < end; ++i) {
                auto& row = group.rows[i - first];
                row.host.visible(true);
                row.property = properties[i];
                row.value = values[i];
                row.differs = *selected == 0 || row.value.value != previous[i].value;
                row.draft = false;
                row.label.text(row.property.label);
                row.keyed.value(row.value.keyed);
                for (auto& field : row.fields) field.visible(false);
                row.boolean.visible(false);
                row.blend.value(
                    continuous(row.value.value) &&
                    row.value.incoming == timeline::Interpolation::linear);
                std::visit(
                    [&](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::same_as<T, bool>)
                            row.boolean.visible(true).value(value);
                        else if constexpr (std::same_as<T, Vec3>) {
                            for (std::size_t n = 0; n < 3; ++n)
                                row.fields[n].visible(true).value(format(value[n]));
                        } else if constexpr (std::same_as<T, f32>)
                            row.fields[0].visible(true).width(218).value(format(value));
                        else if constexpr (std::same_as<T, std::string>)
                            row.fields[0].visible(true).width(218).value(value);
                        else
                            row.fields[0].visible(true).width(218).value(std::to_string(value));
                    },
                    row.value.value);
            }
            first = end;
        }
        for (std::size_t i = group_count; i < groups.size(); ++i) groups[i].host.visible(false);
        rows_dirty = true;
        row_width.reset(); // New values can change the field count/type at the same width.
        update_rows();
        layout();
        dirty = false;
        refresh = false;
    }
    std::optional<TimelineAction> seek(f32 value, bool select,input::Modifiers modifiers={}) {
        if (!std::isfinite(value) || value < 0 || value > snapshot.duration) {
            status.text("Time must lie inside the timeline duration.");
            return {};
        }
        if(select) selection.select(value,click_selection(modifiers,true),visible_times);
        else selection.clear();
        selected=selection.active();
        if(select && selected) value=*selected;
        dirty = false;
        refresh = true;
        rebuild_list = true;
        snapshot.time = value;
        snapshot.paused = true;
        playhead.value(value);
        time.value(format(value));
        inspect();
        update_list();
        return TimelineAction{.kind = TimelineAction::Kind::seek, .time = value};
    }
    std::optional<TimelineAction> edit() {
        if (!selected || !snapshot.paused || snapshot.time != *selected)
            return {};
        const auto destination = number(key_time.getText());
        if (!destination || *destination < 0 || *destination > snapshot.duration) {
            status.text("Timestamp must lie inside the timeline duration.");
            return {};
        }
        TimelineAction action{.kind = TimelineAction::Kind::edit,
                              .time = *selected,
                              .destination = *destination,
                              .name = std::string(name.getText()),
                              .values = {}};
        for (auto& row : active_rows()) {
            auto field = read(row);
            if (!field) {
                status.text("Values must be finite numbers.");
                return {};
            }
            if (field->value != row.value.value || field->keyed != row.value.keyed ||
                field->incoming != row.value.incoming)
                action.values.push_back(std::move(*field));
        }
        awaiting = *destination;
        dirty = false;
        return action;
    }
    std::optional<TimelineAction> edit_range(KeyframeRangeMenu::Range range) {
        if (!selected || !snapshot.paused || snapshot.time != *selected) return {};
        if (number(key_time.getText()) != selected || name.getText() != snapshot.key_name(*selected)) {
            range_menu.error("Apply name/timestamp edits to the single keyframe first."); return {};
        }
        TimelineAction action{.kind = TimelineAction::Kind::apply_range,
                              .time = range.first, .destination = range.last};
        for (const auto& row : active_rows()) {
            const auto field = read(row);
            if (!field) { range_menu.error("Values must be finite numbers."); return {}; }
            if (auto change = keyframe_change(row.value, *field)) action.changes.push_back(std::move(*change));
        }
        if (action.changes.empty()) {
            range_menu.error("Edit a keyframe property before applying to a range."); return {};
        }
        awaiting = selected;
        dirty = false;
        return action;
    }
};

TimelinePanel::TimelinePanel(ui::Container strip, ui::Container list, ui::Container inspector,
                             ui::Container actions, ui::Container range_menu)
    : impl_(std::make_unique<Impl>(strip, list, inspector, actions, range_menu)) {
}
TimelinePanel::~TimelinePanel() = default;
TimelinePanel::TimelinePanel(TimelinePanel&&) noexcept = default;
TimelinePanel& TimelinePanel::operator=(TimelinePanel&&) noexcept = default;
void TimelinePanel::show(const State& state) {
    auto& p = *impl_;
    const bool revised = !p.initialized || p.snapshot.revision != state.document.revision;
    if (revised) {
        ++p.statistics.document_refreshes;
        p.snapshot.revision = state.document.revision;
        std::map<BlueprintId, std::string_view> blueprint_names;
        for (const auto& blueprint : blueprint_catalog(state)) blueprint_names.emplace(blueprint.id, blueprint.name);
        std::map<u64, Impl::Snapshot::ObjectInfo> objects;
        objects.emplace(camera_animation_object, Impl::Snapshot::ObjectInfo{"Animation camera", "Animation camera"});
        for (const auto& instance : state.document.instances) {
            const auto blueprint = blueprint_names.find(instance.blueprint);
            const auto identifier = blueprint == blueprint_names.end() ? instance.name
                : std::string(blueprint->second) + " / " + instance.name;
            objects.emplace(instance.id, Impl::Snapshot::ObjectInfo{instance.name, identifier});
        }
        const bool names_changed = objects != p.snapshot.objects;
        p.snapshot.objects = std::move(objects);
        std::erase_if(p.expansion, [&](const auto& entry) { return !p.snapshot.objects.contains(entry.first); });
        p.snapshot.animation = state.document.timeline;
        p.snapshot.names = state.document.keyframe_names;
        auto properties = animation_properties(state);
        const bool schema_changed =
            properties.size() != p.properties.size() ||
            !std::equal(properties.begin(), properties.end(), p.properties.begin(),
                        [](const auto& a, const auto& b) {
                            return a.target == b.target && a.label == b.label && a.layer == b.layer;
                        });
        p.properties = std::move(properties);
        p.times = keyframe_times(state);
        if (schema_changed || names_changed || !p.layer)
            p.refresh_filters();
        p.rebuild_list = true;
        if (schema_changed) {
            // Instance deletion invalidates drafts for its properties. Never
            // keep a removed object's rows alive solely because a field was focused.
            p.dirty = false;
            p.refresh = true;
        } else if (!p.dirty && !p.editing())
            p.refresh = true;
    }
    if (!p.initialized || p.snapshot.duration != state.document.timeline_duration) {
        p.playhead.range(0, state.document.timeline_duration);
        p.dragging = false;
    }
    p.snapshot.duration = state.document.timeline_duration;
    p.snapshot.time = state.viewport.time;
    p.snapshot.paused = state.viewport.paused;
    p.controls.enabled(p.selected && *p.selected == state.viewport.time && state.viewport.paused);
    p.initialized = true;
    p.playhead.value(std::clamp(state.viewport.time, 0.0F, state.document.timeline_duration));
    const auto time_text = format(state.viewport.time);
    const auto duration_text = format(state.document.timeline_duration);
    if (!p.time.isFocused() && p.time.getText() != time_text)
        p.time.value(time_text);
    if (!p.duration.isFocused() && p.duration.getText() != duration_text)
        p.duration.value(duration_text);
    if (p.awaiting) {
        p.range_menu.close();
        if (p.selected != p.awaiting) p.dirty = false;
        p.selected = p.awaiting;
        p.selection.select(*p.awaiting);
        p.awaiting.reset();
        p.refresh = true;
        p.rebuild_list = true;
    }
    p.selection.retain([&](f32 time){return p.exists(time);});
    if (p.selected!=p.selection.active()) {
        p.selected=p.selection.active();
        // Undo or deletion can remove the keyframe while a field still has an
        // unsent draft. There is no longer a target to preserve that draft for.
        p.dirty = false;
        p.refresh = true;
        p.rebuild_list = true;
    }
    if (p.refresh && !p.dirty)
        p.inspect();
    if (!p.dirty) p.status.text(p.selected && *p.selected == state.viewport.time && at_paused_keyframe(state)
        ? "Click to select / Shift-click: range / Ctrl-click: toggle / Delete preserves time zero"
        : "Read-only pose / Select a keyframe or insert one to edit / Editor-camera navigation is available");
    p.controls.enabled(p.selected && *p.selected == state.viewport.time && state.viewport.paused);
    const bool editable = p.selected && *p.selected == state.viewport.time && state.viewport.paused;
    p.apply.enabled(editable); p.apply_range.enabled(editable);
    if (!editable) p.range_menu.close();
    p.update_list();
    p.layout();
}
void TimelinePanel::clear_selection() {
    auto& p = *impl_;
    if (!p.selected && p.selection.size() == 0 && !p.awaiting && !p.dirty) return;
    p.selected.reset();
    p.range_menu.close();
    p.selection.clear();
    p.awaiting.reset();
    p.dirty = false;
    p.refresh = true;
    p.rebuild_list = true;
    p.inspect();
    p.update_list();
}
void TimelinePanel::select_keyframe(vng::f32 time) {
    impl_->awaiting=time;
}
void TimelinePanel::focus_object(u64 object) {
    auto& p = *impl_;
    if (!p.selected || !object) return;
    const auto groups = p.active_groups();
    const auto group = std::ranges::find(groups, object, &Impl::Group::object);
    if (group == groups.end() || !group->row_count) return;
    p.selected_objects.insert(object);
    p.expansion.erase(object);
    if (p.object->value() && p.object->value() != object) p.object->value(0);
    p.rows_dirty = true;
    p.update_rows();
    group->host.reveal();
    group->rows.front().keyed.focus();
}
void TimelinePanel::selected_objects(std::span<const u64> objects) {
    auto& p = *impl_;
    std::set<u64> next(objects.begin(), objects.end());
    if (p.selected_objects == next) return;
    // Manual overrides for unrelated objects survive; auto-opened entries
    // collapse when deselected, including one manually toggled while selected.
    for (auto id : p.selected_objects) if (!next.contains(id)) p.expansion.erase(id);
    for (auto id : next) if (!p.selected_objects.contains(id)) p.expansion.erase(id);
    p.selected_objects = std::move(next);
    p.rows_dirty = true;
    p.update_rows();
}
void TimelinePanel::select_keyframes(std::span<const f32> times) {
    auto& p=*impl_;
    p.awaiting.reset();p.selection.select(times);p.selected=p.selection.active();
    p.dirty=false;p.refresh=p.rebuild_list=true;
}
std::optional<TimelineAction> TimelinePanel::poll(std::span<const input::Event> unhandled,
                                                  std::span<const input::Event> raw) {
    auto& p = *impl_;
    p.handled = false;
    if (!p.initialized)
        return {};
    if (p.layer->changedValue()) {
        p.rebuild_list = true;
        p.update_list();
    }
    if (p.object->changedValue()) {
        p.rows_dirty = true;
        p.update_rows();
    }
    p.dirty |= p.name.changedText().has_value() || p.key_time.changedText().has_value();
    for (auto& row : p.active_rows()) {
        const bool edited = row.keyed.changedValue().has_value() || row.blend.changedValue().has_value() ||
                   row.boolean.changedValue().has_value() ||
                   std::ranges::any_of(row.fields, [](const auto& field) {
                       return field.changedText().has_value();
                   });
        p.dirty |= edited;
        if (edited) {
            p.rows_dirty = true;
            const auto value = p.read(row);
            row.draft = !value || value->value != row.value.value || value->keyed != row.value.keyed ||
                        value->incoming != row.value.incoming;
        }
    }
    for (auto& group : p.active_groups()) if (group.toggle.clicked()) {
        p.expansion[group.object] = !p.expanded(group.object);
        p.rows_dirty = true;
    }
    p.update_rows();
    p.layout();
    if (p.apply_range.clicked()) {
        if (p.range_menu.opened()) p.range_menu.close();
        else if (p.selected) {
            auto first = *p.selected, last = p.times.back();
            if (p.selection.size() > 1) {
                first = *std::ranges::min_element(p.selection.items());
                last = *std::ranges::max_element(p.selection.items());
            }
            p.range_menu.open(p.times, {first, last});
        }
    }
    if (auto range = p.range_menu.poll(raw)) return p.edit_range(*range);
    if (p.range_menu.opened()) return {};
    for (const auto& group : p.active_groups()) if (group.identifier.clicked())
        return TimelineAction{.kind = TimelineAction::Kind::select_object,
            .object = group.object,
            .selection_mode = click_selection(click_modifiers(raw, group.identifier.bounds()), true)};
    if (p.add.clicked()) {
        p.awaiting = p.snapshot.time;
        p.dirty = false;
        return TimelineAction{.kind = TimelineAction::Kind::add, .time = p.snapshot.time};
    }
    if (p.selected && p.erase.clicked())
        return TimelineAction{.kind = TimelineAction::Kind::erase, .time = *p.selected,
            .selection={p.selection.items().begin(),p.selection.items().end()}};
    if (p.selected && (p.apply.clicked() || p.name.submittedText() || p.key_time.submittedText()))
        return p.edit();
    if (const auto duration = p.duration.submittedText()) {
        const auto value = number(*duration);
        if (!value || *value < .1F || *value > timeline::max_time) {
            p.status.text("Duration must be between 0.1 and 86400 seconds.");
            return {};
        }
        return TimelineAction{.kind = TimelineAction::Kind::duration, .time = *value};
    }
    if (const auto time = p.time.submittedText()) {
        const auto value = number(*time);
        if (!value) {
            p.status.text("Time must be a finite number.");
            return {};
        }
        return p.seek(*value, false);
    }
    if (const auto value = p.playhead.changedValue())
        return p.seek(*value, false);
    for (const auto& [time, button] : p.entries)
        if (button.clicked())
            return p.seek(time, true,click_modifiers(raw,button.bounds()));
    const auto bounds = p.canvas.bounds();
    const auto available = [&](const input::Event& event) {
        return std::ranges::any_of(unhandled, [&](const auto& value) {
            return value.kind == event.kind && value.position == event.position &&
                   value.button == event.button;
        });
    };
    std::optional<TimelineAction> action;
    for (const auto& event : raw) {
        if (event.kind == input::EventKind::focus_lost ||
            (event.kind == input::EventKind::key_down && event.key == input::Key::escape)) {
            p.dragging = false;
            continue;
        }
        if (!finite(event.position))
            continue;
        if (p.dragging) {
            if (event.kind == input::EventKind::pointer_move ||
                (event.kind == input::EventKind::pointer_up && event.button == 0)) {
                p.handled = true;
                action =
                    p.seek(std::clamp((event.position.x - p.drag_bounds.x) / p.drag_bounds.width,
                                      0.0F, 1.0F) *
                               p.snapshot.duration,
                           false);
                if (event.kind == input::EventKind::pointer_up)
                    p.dragging = false;
            }
            continue;
        }
        if (event.kind != input::EventKind::pointer_down || event.button != 0 ||
            !bounds.contains(event.position) || !available(event) || bounds.width <= 0)
            continue;
        p.handled = true;
        std::optional<f32> hit;
        f32 nearest = 9;
        if (event.position.y >= bounds.y + 24)
            for (auto time : p.visible_times) {
                const auto distance =
                    std::abs(event.position.x -
                             (bounds.x + time / p.snapshot.duration * bounds.width));
                if (distance < nearest) {
                    hit = time;
                    nearest = distance;
                }
            }
        if (hit)
            action = p.seek(*hit, true,event.modifiers); // Markers select only, never drag/move.
        else {
            p.dragging = true;
            p.drag_bounds = bounds;
            action = p.seek(std::clamp((event.position.x - bounds.x) / bounds.width, 0.0F, 1.0F) *
                                p.snapshot.duration,
                            false);
        }
    }
    return action;
}
void TimelinePanel::compact(bool enabled) {
    impl_->canvas.height(enabled ? 32.F : 72.F);
    impl_->status.visible(!enabled);
}
void TimelinePanel::append(ui::DrawList& draw) const {
    const auto& p = *impl_;
    const auto anchor = std::ranges::find_if(draw.commands, [&](const auto& command) {
        const auto* image = std::get_if<ui::ImageDraw>(&command);
        return image && image->pixels == p.anchor;
    });
    if (anchor == draw.commands.end() || !p.initialized)
        return;
    const auto bounds = p.canvas.bounds();
    const auto clip = std::get<ui::ImageDraw>(*anchor).clip;
    std::optional<ui::TextDraw> text_style;
    for (const auto& command : draw.commands)
        if (const auto* text = std::get_if<ui::TextDraw>(&command);
            text && text->text == "TIMELINE")
            text_style = *text;
    std::vector<ui::DrawCommand> commands;
    const auto box = [&](ui::Rect rect, Vec4 color, f32 radius = 0) {
        commands.emplace_back(ui::BoxDraw{rect, clip, color, {}, radius, 0});
    };
    box(bounds, {.025F, .035F, .052F, 1});
    for (int i = 0; i <= 10; ++i) {
        const auto x = bounds.x + bounds.width * static_cast<f32>(i) / 10;
        box({x, bounds.y, 1, bounds.height}, {.12F, .17F, .22F, 1});
        if (text_style) {
            auto text = *text_style;
            text.text = format(p.snapshot.duration * static_cast<f32>(i) / 10);
            text.size = 14;
            text.clip = clip;
            text.position = {i == 10 ? x - 38 : x + 4, bounds.y + 2};
            commands.emplace_back(std::move(text));
        }
    }
    for (auto time : p.visible_times) {
        const auto x = bounds.x + time / p.snapshot.duration * bounds.width;
        box({x - 5, bounds.y + 32, 10, 24},
            p.selection.contains(time) ? Vec4{1, .76F, .2F, 1} : Vec4{.27F, .72F, .95F, 1}, 4);
    }
    const auto x =
        bounds.x +
        std::clamp(p.snapshot.time / p.snapshot.duration, 0.0F, 1.0F) * bounds.width;
    box({x, bounds.y, 1, bounds.height}, {1, .42F, .08F, 1});
    const auto index = static_cast<std::size_t>(anchor - draw.commands.begin());
    draw.commands.erase(anchor);
    draw.commands.insert(draw.commands.begin() + static_cast<std::ptrdiff_t>(index),
                         std::make_move_iterator(commands.begin()),
                         std::make_move_iterator(commands.end()));
}
std::optional<f32> TimelinePanel::selected_keyframe() const noexcept {
    return impl_->selected;
}
std::span<const f32> TimelinePanel::selected_keyframes() const noexcept {return impl_->selection.items();}
bool TimelinePanel::dragging() const noexcept {
    return impl_->dragging || impl_->playhead.isPressed();
}
bool TimelinePanel::handledPointer() const noexcept {
    return impl_->handled;
}
void TimelinePanel::cancel() noexcept {
    impl_->dragging = false;
    impl_->handled = false;
    impl_->range_menu.close();
}
void TimelinePanel::reset() noexcept {
    cancel();
    impl_->initialized = false;
    impl_->selected.reset();
    impl_->expansion.clear();
    impl_->selection.clear();
    impl_->awaiting.reset();
    impl_->dirty = false;
    impl_->refresh = true;
    impl_->rebuild_list = true;
}
void TimelinePanel::error(std::string_view message) {
    impl_->awaiting.reset();
    impl_->dirty = true;
    impl_->status.text(message);
    if (impl_->range_menu.opened()) impl_->range_menu.error(message);
}
void TimelinePanel::layout_menu(Vec2 size) { impl_->range_menu.layout(size, impl_->apply_range.bounds()); }
void TimelinePanel::actions_enabled(bool enabled) {
    impl_->action_host.enabled(enabled);
    if (!enabled) impl_->range_menu.close();
}
bool TimelinePanel::menu_open() const { return impl_->range_menu.opened(); }
bool TimelinePanel::menu_contains(Vec2 point) const { return impl_->range_menu.contains(point); }
void TimelinePanel::close_menu() { impl_->range_menu.close(); }
TimelinePanel::Statistics TimelinePanel::statistics() const { return impl_->statistics; }
} // namespace editor_example
