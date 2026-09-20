#include "../../examples/editor/app.hpp"
#include "../../examples/editor/automation.hpp"
#include "../../examples/editor/mesh_tools.hpp"
#include "../../examples/editor/rotation_tool.hpp"
#include "../../examples/editor/scale_tool.hpp"
#include "../../examples/editor/translation_tool.hpp"
#include "../../examples/editor/surface_part_tool.hpp"
#include "../../examples/editor/world_bounds_tool.hpp"
#include "../../examples/editor/cage_tool.hpp"
#include "../../examples/editor/blueprint_gizmos.hpp"
#include "../../examples/editor/rotation_math.hpp"
#include "../../examples/editor/animation.hpp"
#include "../../examples/editor/effect_presets.hpp"
#include "../../examples/editor/keyframes.hpp"
#include "../../examples/editor/settings.hpp"
#include "../../examples/editor/ui_scale.hpp"
#include "../../examples/editor/transform_pivot.hpp"
#include "../../examples/editor/selection.hpp"
#include "../../examples/editor/view_selector.hpp"
#include "../../examples/support/presentation.hpp"
#include "../../examples/support/asteroid_scene.hpp"
#include "../../examples/support/earth_assets.hpp"
#include "../../examples/support/earth_scene.hpp"
#include "../../examples/editor/scene_file.hpp"
#include <vng/ui/inspection.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {
namespace project = editor_example;
namespace ui = vng::ui;
namespace input = vng::input;
namespace fs = std::filesystem;
using namespace vng;
using Observation = project::EditorObservation;
using Clock = std::chrono::steady_clock;
using Role = ui::WidgetRole;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
template<class T, class E> T take(std::expected<T, E> result) {
    if (!result) throw std::runtime_error(result.error().message);
    if constexpr (!std::same_as<T, void>) return std::move(*result);
}
std::string quoted(std::string_view text) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : text) {
        if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    return out.str() + '"';
}
std::string html(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}
ui::Rect intersection(ui::Rect a, ui::Rect b) {
    const auto x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0F, std::min(a.x + a.width, b.x + b.width) - x),
            std::max(0.0F, std::min(a.y + a.height, b.y + b.height) - y)};
}
Vec2 center(ui::Rect r) { return {r.x + r.width * .5F, r.y + r.height * .5F}; }

const ui::TextDraw* fps_text(const ui::DrawList& list) {
    for (const auto& command : list.commands)
        if (const auto* text = std::get_if<ui::TextDraw>(&command);
            text && text->text.starts_with("Preview: ")) return text;
    return nullptr;
}

// Locators use public semantic snapshots, never widget handles or native
// internals. An optional heading/dropdown scope disambiguates repeated labels.
struct Locator {
    Role role;
    std::string label, scope;
};
Locator button(std::string label, std::string scope = {}) { return {Role::button, std::move(label), std::move(scope)}; }
Locator field(std::string label, std::string scope = {}) { return {Role::text_field, std::move(label), std::move(scope)}; }
Locator dropdown(std::string label) { return {Role::dropdown, std::move(label), {}}; }
Locator option(std::string label, std::string scope) {
    if(scope=="Gizmo")for(const auto& description:project::gizmo_descriptions)
        if(label==description.label){label=project::gizmo_choice_label(description.mode);break;}
    return {Role::option, std::move(label), std::move(scope)};
}

class Driver final : public project::Automation {
public:
    explicit Driver(fs::path directory, bool fleet = false, bool multi = false, bool popout = false, bool earth = false) : directory_(std::move(directory)) {
        if(earth) earth_workflow(); else if(popout) popout_workflow(); else if(multi) multiselect_workflow(); else if (fleet) fleet_timeline_workflow(); else workflow();
    }
    void input(input::Frame& frame) override {
        if (viewport_events_) { frame.events.clear(); frame.focused=false; return; }
        frame.events = std::move(events_);
        events_.clear();
        frame.focused = true;
        frame.overflow = std::exchange(overflow_next_input_, false);
        if (std::exchange(minimize_next_input_, false)) frame.framebuffer = {};
        frame.pointer = pointer_;
        frame.keys.fill(false);
    }
    void viewport_input(input::Frame& frame) override {
        if (!viewport_events_) return;
        frame.events=std::move(events_); events_.clear(); frame.focused=true;
        frame.pointer=pointer_; frame.keys.fill(false);
    }
    void observe(const Observation& observation) override {
        try {
            tree_ = take(observation.screen.inspect());
            detached_=observation.viewport_detached;
            vng::u64 surface_id=1;
            for (const auto* surface : {observation.viewport_screen,observation.viewport_popups}) {
                if (!surface) continue;
                auto view=take(surface->inspect());
                for (auto widget:view.widgets) {
                    widget.id |= surface_id<<40;
                    if (widget.parent) widget.parent |= surface_id<<40;
                    tree_.widgets.push_back(std::move(widget));
                }
                ++surface_id;
            }
            last_status_ = observation.status;
            last_logs_ = observation.worker_logs;
            if (steps_.empty()) { done_ = true; report("passed"); return; }
            auto& step = steps_.front();
            if (!step.started) step.started = Clock::now();
            const auto timeout = std::chrono::seconds{results_.empty() ? 120 : 30};
            require(Clock::now() - *step.started < timeout,
                    "Timed out waiting for step: " + step.name + " / " + last_status_);
            if (step.run(observation)) {
                const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - *step.started).count();
                results_.emplace_back(step.name, elapsed);
                std::cout << "PASS " << step.name << " (" << elapsed << " ms)\n";
                steps_.pop_front();
            }
        } catch (const std::exception& error) {
            failure_ = error.what();
            try { checkpoint(observation, "failure"); } catch (...) {}
            report("failed");
            throw;
        }
    }
    bool finished() const override { return done_; }
    void startup_failure(std::string message) { failure_ = std::move(message); report("failed"); }
    void skipped(std::string message) { last_status_ = std::move(message); report("skipped"); }

private:
    struct Step {
        std::string name;
        std::function<bool(const Observation&)> run;
        std::optional<Clock::time_point> started;
    };
    fs::path directory_;
    ui::Inspection tree_;
    std::deque<Step> steps_;
    std::vector<input::Event> events_;
    Vec2 pointer_{};
    std::vector<std::pair<std::string, double>> results_;
    std::string failure_, last_status_, last_logs_;
    bool done_{}, minimize_next_input_{}, overflow_next_input_{};
    bool detached_{}, viewport_events_{};
    u32 imported_{}, sibling_{};
    project::BlueprintId imported_blueprint_{project::BlueprintId::mesh};
    std::string imported_view_label_;
    std::size_t blueprints_{};
    u32 scene_selection_before_blueprint_{};
    Vec3 blueprint_vertex_{};
    Vec3 region_vertex_{};
    std::vector<u32> mesh_selection_;
    u64 mesh_selection_revision_{};
    Vec3 original_position_{}, committed_position_{}, saved_position_{};
    std::vector<std::byte> original_pixels_, committed_pixels_, normal_pixels_;
    std::vector<std::byte> before_scale_pixels_, scaled_pixels_;
    std::optional<content::vmesh::Document> imported_document_;
    std::optional<content::vmesh::Document> builtin_document_;
    Vec2 drag_start_{}, drag_direction_{};
    Vec3 drag_original_{};
    Vec3 forward_axis_{}, forward_before_{}, forward_after_{};
    std::vector<std::byte> drag_pixels_;
    u64 input_frame_{};
    bool selection_dirty_{};
    project::CameraPose animation_camera_{};
    project::CameraPose zoom_before_{};
    project::WorldBounds bounds_before_{}, bounds_after_{};
    u64 zoom_revision_{},zoom_sequence_{};
    u64 sidebar_bar_{}, instance_bar_{};
    f32 sidebar_before_{}, list_before_{};
    u64 scroll_revision_{};
    Vec3 rotation_before_{}, rotation_after_{};
    Vec2 rotation_start_{}, rotation_end_{};
    std::vector<std::byte> rotation_pixels_;
    Vec2 scale_pointer_{};
    f32 gizmo_scale_{};
    u32 pasted_instance_{};
    u64 fleet_revision_{};
    std::vector<project::SceneInstance> group_before_;
    std::vector<u64> keyframe_widget_ids_;
    project::SceneValues insertion_pose_;
    project::CameraPose insertion_camera_;

    const ui::WidgetSnapshot* node(u64 id) const {
        const auto found = std::ranges::find(tree_.widgets, id, &ui::WidgetSnapshot::id);
        return found == tree_.widgets.end() ? nullptr : &*found;
    }
    bool inside(const ui::WidgetSnapshot& candidate, u64 parent) const {
        for (auto current = &candidate; current; current = node(current->parent)) {
            if (current->id == parent) return true;
            if (!current->parent) break;
        }
        return false;
    }
    const ui::WidgetSnapshot* find(const Locator& locator, bool visible_only = true, bool include_hidden = false) const {
        std::vector<u64> scopes;
        if (!locator.scope.empty())
            for (const auto& item : tree_.widgets)
                if ((include_hidden || (visible_only ? item.visible : item.in_layout)) && (item.label == locator.scope || item.text == locator.scope))
                    scopes.push_back(item.role == Role::dropdown ? item.id : item.parent);
        const ui::WidgetSnapshot* result{};
        for (const auto& item : tree_.widgets) {
            if ((!include_hidden && !(visible_only ? item.visible : item.in_layout)) || !item.enabled || item.role != locator.role) continue;
            if (!locator.scope.empty() && std::ranges::none_of(scopes, [&](u64 scope) { return inside(item, scope); })) continue;
            bool matches = item.label == locator.label || item.text == locator.label || item.placeholder == locator.label;
            // Unnamed inputs commonly follow a human-visible label in a row.
            if (!matches && locator.role == Role::text_field && item.label.empty()) {
                const ui::WidgetSnapshot* preceding_label{};
                for (const auto& sibling : tree_.widgets) {
                    if (sibling.id == item.id) break;
                    if (sibling.parent == item.parent && sibling.role == Role::label)
                        preceding_label = &sibling;
                }
                matches = preceding_label && preceding_label->text == locator.label;
            }
            if (!matches) continue;
            require(!result, "Ambiguous locator: " + locator.label + " within " + locator.scope);
            result = &item;
        }
        return result;
    }
    void drag_scrollbar(const ui::WidgetSnapshot& bar, f32 value) {
        require(bar.thumb && bar.maximum && *bar.maximum > 0, "Expected usable scroll thumb");
        const auto start = center(*bar.thumb);
        const auto travel = bar.bounds.height - bar.thumb->height;
        const Vec2 end{start.x, bar.bounds.y + bar.thumb->height * .5F +
                                   std::clamp(value / *bar.maximum, 0.F, 1.F) * travel};
        pointer(input::EventKind::pointer_move, start);
        pointer(input::EventKind::pointer_down, start);
        pointer(input::EventKind::pointer_move, end);
        pointer(input::EventKind::pointer_up, end);
    }
    const ui::WidgetSnapshot* actionable(const Locator& locator) {
        const auto* target = find(locator, false, locator.role==Role::option);
        // Long blueprint-part menus use the same scrollable popup as other
        // dropdowns. Reveal options through real wheel events, not widget edits.
        if(target && target->role==Role::option && !target->visible) {
            for(const auto& option:tree_.widgets)if(option.role==Role::option &&
                option.parent==target->parent && option.visible && option.option_index && target->option_index) {
                pointer_=center(option.bounds);
                events_.push_back({.kind=input::EventKind::scroll,.position=pointer_,
                    .scroll={0,*target->option_index>*option.option_index?-6.F:6.F}});
                return nullptr;
            }
        }
        const bool camera_control=locator.label=="Orbit" || locator.label=="Elevation" || locator.label=="Orbit distance" ||
            locator.label=="Zoom" || locator.label=="Scroll moves camera" || locator.label=="Walk camera" ||
            locator.label=="Stop walking" || locator.label=="Edit animation camera";
        const auto* owner=target;
        while(owner && owner->parent) owner=node(owner->parent);
        if(camera_control && (!owner || !owner->visible)) {
            const auto* opener=actionable(button("Camera settings"));
            if(opener) click_at(center(intersection(opener->bounds,opener->clip)));
            return nullptr;
        }
        if (!target) {
            // Reach hidden pages through actual tab clicks, preserving the
            // same end-to-end input path as a user. No widget mutations.
            const auto* hidden=find(locator,false,true);
            if (!hidden) return nullptr;
            for (const auto& heading : tree_.widgets) {
                std::string tab;
                u64 page=heading.parent;
                if(heading.role==Role::label && (heading.text=="More..." || heading.text=="Tools...")) tab=heading.text;
                else if(heading.text=="SCENE INSTANCES") {tab="Scene";if(const auto* section=node(page))page=section->parent;}
                else if(heading.text.starts_with("KEYFRAMES /")) tab="Scene";
                else if(heading.text.starts_with("KEYFRAME /")) tab="Keyframe values";
                else if(heading.text=="REGION / scene annotation") tab="Instance properties";
                else if(heading.text=="BLUEPRINT / local XYZ") {
                    if(const auto* container=node(page)) page=container->parent;
                    tab=find(button("Blueprint geometry"))?"Blueprint geometry":"Instance properties";
                }
                if(!tab.empty() && inside(*hidden,page)) {
                    if(const auto* opener=find(button(tab));opener && !opener->selected)
                        click_at(center(opener->bounds));
                    return nullptr;
                }
            }
            return nullptr;
        }
        const auto shown = intersection(target->bounds, target->clip);
        if (target->visible && shown.height >= target->bounds.height - 1 &&
            shown.width >= target->bounds.width - 1) return target;
        // Reveal through real thumb gestures, never mutate widgets/scene state.
        for (auto ancestor = node(target->parent); ancestor; ancestor = node(ancestor->parent)) {
            for (const auto& bar : tree_.widgets) {
                if (bar.role != Role::scrollbar || bar.parent != ancestor->id || !bar.visible ||
                    !bar.enabled || !bar.thumb || !bar.maximum || *bar.maximum <= 0) continue;
                const auto thumb_clip = intersection(*bar.thumb, bar.clip);
                if (thumb_clip.height < bar.thumb->height - 1) continue;
                f32 delta{};
                if (target->bounds.y < bar.bounds.y) delta = target->bounds.y - bar.bounds.y;
                else if (target->bounds.y + target->bounds.height > bar.bounds.y + bar.bounds.height)
                    delta = target->bounds.y + target->bounds.height - bar.bounds.y - bar.bounds.height;
                const auto next = std::clamp(bar.number.value_or(0) + delta, 0.F, *bar.maximum);
                if (std::abs(next - bar.number.value_or(0)) < .1F) continue;
                drag_scrollbar(bar, next);
                return nullptr;
            }
        }
        return nullptr;
    }
    void pointer(input::EventKind kind, Vec2 point, u32 mouse_button = 0, input::Modifiers modifiers = {}) {
        pointer_ = point;
        events_.push_back({.kind = kind, .position = point, .button = mouse_button, .modifiers=modifiers});
    }
    void click_at(Vec2 point) {
        pointer(input::EventKind::pointer_move, point);
        pointer(input::EventKind::pointer_down, point);
        pointer(input::EventKind::pointer_up, point);
    }
    void key(input::Key value, input::Modifiers modifiers = {}) {
        events_.push_back({.kind = input::EventKind::key_down, .position=pointer_, .key = value, .modifiers = modifiers});
        events_.push_back({.kind = input::EventKind::key_up, .position=pointer_, .key = value, .modifiers = modifiers});
    }
    void add(std::string name, std::function<bool(const Observation&)> run) {
        steps_.push_back({std::move(name), std::move(run), {}});
    }
    void wait(std::string name, std::function<bool(const Observation&)> predicate) { add(std::move(name), std::move(predicate)); }
    void click(Locator locator) {
        add("Click " + locator.label, [this, locator](const Observation&) {
            const auto* target = actionable(locator);
            if (!target) return false;
            viewport_events_=detached_ && (target->id & (vng::u64{3}<<40));
            const auto bounds = intersection(target->bounds, target->clip);
            require(bounds.width > 2 && bounds.height > 2, "Control has no usable visible hit area");
            click_at(center(bounds));
            return true;
        });
        if(locator.role==Role::checkbox && (locator.label=="Scroll moves camera" || locator.label=="Edit animation camera"))
            click(button("Close camera settings"));
    }
    void fill(Locator locator, std::string value) {
        add("Fill " + locator.label, [this, locator, value = std::move(value)](const Observation&) {
            const auto* target = actionable(locator);
            if (!target) return false;
            click_at(center(intersection(target->bounds, target->clip)));
            key(input::Key::a, {.control = true});
            events_.push_back({.kind = input::EventKind::text, .text = value});
            return true;
        });
    }
    static bool ready(const Observation& o) {
        return o.preview_ready && o.image_revision == o.state.document.revision && !o.modal;
    }
    static Vec3 position(const Observation& o, u32 id) {
        const auto* instance = project::find_instance(o.state, id);
        require(instance != nullptr, "Expected scene instance is missing");
        return project::evaluate_instance(o.state, *instance, o.state.viewport.time).transform.position;
    }
    static Vec2 project_point(const Observation& o, Vec3 point) {
        const auto snapshot = take(project::camera(o.state).snapshot(o.preview_extent));
        Vec4 clip{};
        for (std::size_t row = 0; row < 4; ++row) {
            clip[row] = snapshot.view_projection[3][row];
            for (std::size_t column = 0; column < 3; ++column)
                clip[row] += snapshot.view_projection[column][row] * point[column];
        }
        require(clip.w > 0, "Test point is behind camera");
        return {o.viewport.x + (clip.x / clip.w + 1) * .5F * o.viewport.width,
                o.viewport.y + (1 - clip.y / clip.w) * .5F * o.viewport.height};
    }
    static bool equivalent(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](auto x, auto y) {
            return std::abs(std::to_integer<int>(x) - std::to_integer<int>(y)) <= 1;
        });
    }
    static void visible_pixels(const Observation& o) {
        require(o.preview_pixels && !o.preview_pixels->pixels.empty(), "No real worker image");
        std::size_t bright{};
        for (std::size_t i = 0; i < o.preview_pixels->pixels.size(); i += 4)
            bright += std::to_integer<unsigned>(o.preview_pixels->pixels[i]) > 90 ||
                      std::to_integer<unsigned>(o.preview_pixels->pixels[i + 1]) > 90 ||
                      std::to_integer<unsigned>(o.preview_pixels->pixels[i + 2]) > 90;
        require(bright > 100, "Preview contains no substantial visible geometry");
    }
    static void blueprint_projection(const Observation& o, project::BlueprintId blueprint) {
        visible_pixels(o);
        require(o.state.viewport.mode == project::ViewMode::mesh && o.state.viewport.inspected_mesh == blueprint,
                "Blueprint editor retained the wrong explicit target");
        const auto* geometry = project::mesh_edit_geometry(o.state, blueprint);
        require(geometry && project::editable_mesh(o.state) == geometry,
                "Vertex tools resolved geometry through the scene selection instead of the blueprint");
        const auto projected = project::project_vertices(o.state, o.preview_extent);
        require(projected.size() == geometry->size() &&
                    std::ranges::any_of(projected, [](const auto& vertex) { return vertex.has_value(); }),
                "Selected blueprint has no usable local vertex projections");
    }
    static std::string mesh_view_label(const project::State& state, project::BlueprintId blueprint) {
        const auto choices = project::view_choices(state);
        const auto entry = std::ranges::find_if(choices, [&](const auto& choice) {
            return choice.value == project::ViewSelection{project::ViewMode::mesh, blueprint};
        });
        require(entry != choices.end(), "Expected mesh is absent from the View choices");
        const auto catalog = project::blueprint_catalog(state);
        const auto asset = std::ranges::find(catalog, blueprint, &project::Blueprint::id);
        require(asset != catalog.end(), "Expected reusable blueprint is missing from catalog");
        std::string name{asset->name};
        if (blueprint == project::BlueprintId::mesh) {
            const auto& metadata = state.document.mesh.document().metadata;
            if (const auto found = metadata.find("name"); found != metadata.end() &&
                !found->second.empty() && found->second.size() <= 256)
                name = found->second;
        }
        require(entry->label.starts_with("Mesh: " + name), "View mesh caption does not identify its asset");
        return entry->label;
    }
    void assert_view_menu(const Observation& o) const {
        const auto* view = find(dropdown("View"));
        require(view != nullptr, "The top View dropdown is missing");
        const auto choices = project::view_choices(o.state);
        std::size_t option_count{};
        for (const auto& item : tree_.widgets) {
            require(item.role != Role::dropdown || item.label != "Mesh blueprint",
                    "A second right-panel mesh selector is still present");
            if (item.role == Role::option && item.visible && inside(item, view->id)) ++option_count;
        }
        require(option_count == choices.size(), "Actual View popup has missing or stale destinations");
        for (const auto& choice : choices)
            require(find(option(choice.label, "View")) != nullptr, "Expected destination is missing from View popup");
        require(!find(option("Blueprint mesh", "View")), "View still exposes an indirect generic mesh destination");
    }
    void checkpoint(const Observation& o, std::string_view name) {
        const auto screenshot = take(o.viewport_detached ? o.capture_viewport() : o.capture());
        const auto logical_size=o.viewport_detached ? o.viewport_draw_list->logical_size : tree_.logical_size;
        const auto& scene_draw=o.viewport_detached ? *o.viewport_draw_list : o.draw_list;
        if (o.viewport_detached) {
            const auto controls=take(o.capture());
            take(example::write_rgba8_png(directory_/(std::string(name)+"-controls.png"),controls.extent,
                {reinterpret_cast<const u8*>(controls.pixels.data()),controls.pixels.size()}));
        }
        if(name!="failure" && o.state.viewport.mode==project::ViewMode::mesh && o.preview_ready && !o.modal) {
            std::size_t bright{};
            const auto sx=static_cast<f32>(screenshot.extent.width)/logical_size.x;
            const auto sy=static_cast<f32>(screenshot.extent.height)/logical_size.y;
            for(u32 y=0;y<screenshot.extent.height;++y) for(u32 x=0;x<screenshot.extent.width;++x) {
                if(!o.viewport.contains({static_cast<f32>(x)/sx,static_cast<f32>(y)/sy})) continue;
                const auto at=(std::size_t(y)*screenshot.extent.width+x)*4;
                bright+=std::to_integer<unsigned>(screenshot.pixels[at])>90 ||
                    std::to_integer<unsigned>(screenshot.pixels[at+1])>90 || std::to_integer<unsigned>(screenshot.pixels[at+2])>90;
            }
            require(bright>100,"Composed mesh viewport is blank despite a ready preview");
        }
        const bool surface_handle=o.mesh_part_gizmo&&o.mesh_part_gizmo->visible();
        if (name != "failure" && o.gizmo_visible && !o.rotation_gizmo && !o.scale_gizmo && !surface_handle) {
            std::size_t colored_heads{}, expected_heads{};
            const auto scale_x = static_cast<f32>(screenshot.extent.width) / logical_size.x;
            const auto scale_y = static_cast<f32>(screenshot.extent.height) / logical_size.y;
            for (const auto& command : scene_draw.commands) {
                Vec2 point;
                if(const auto* box=std::get_if<ui::BoxDraw>(&command);
                   box && box->rect.width==10 && box->rect.height==10 && box->radius==5)point=center(box->rect);
                else if(const auto* arrow=std::get_if<ui::TriangleDraw>(&command);
                    arrow && arrow->color.x<.2F && arrow->color.y>.8F && arrow->color.z>.8F) {
                    point={(arrow->points[0].x+arrow->points[1].x+arrow->points[2].x)/3,
                           (arrow->points[0].y+arrow->points[1].y+arrow->points[2].y)/3};
                } else continue;
                if (!o.viewport.contains(point)) continue;
                ++expected_heads;
                const auto x = static_cast<u32>(point.x * scale_x), y = static_cast<u32>(point.y * scale_y);
                if (x >= screenshot.extent.width || y >= screenshot.extent.height) continue;
                const auto index = (std::size_t(y) * screenshot.extent.width + x) * 4;
                const auto r = std::to_integer<unsigned>(screenshot.pixels[index]);
                const auto g = std::to_integer<unsigned>(screenshot.pixels[index + 1]);
                const auto b = std::to_integer<unsigned>(screenshot.pixels[index + 2]);
                colored_heads += (r > 180 && g < 160) || (g > 180 && r < 160) || (b > 180 && r < 160);
            }
            require(expected_heads>0 && colored_heads>=std::min(std::size_t{2},expected_heads),
                "Composed screenshot is missing the displayed gizmo heads");
        }
        if(name!="failure"&&o.gizmo_visible&&surface_handle) {
            const auto p=*o.mesh_part_gizmo->handle();
            const auto sx=static_cast<f32>(screenshot.extent.width)/logical_size.x;
            const auto sy=static_cast<f32>(screenshot.extent.height)/logical_size.y;
            std::size_t cyan{};
            for(int y=int((p.y-12)*sy);y<=int((p.y+12)*sy);++y)
                for(int x=int((p.x-12)*sx);x<=int((p.x+12)*sx);++x) {
                    if(x<0||y<0||x>=int(screenshot.extent.width)||y>=int(screenshot.extent.height))continue;
                    const auto at=(std::size_t(y)*screenshot.extent.width+std::size_t(x))*4;
                    const auto r=std::to_integer<unsigned>(screenshot.pixels[at]);
                    const auto g=std::to_integer<unsigned>(screenshot.pixels[at+1]);
                    const auto b=std::to_integer<unsigned>(screenshot.pixels[at+2]);
                    cyan+=r<160&&g>180&&b>200;
                }
            require(cyan>6,"Composed screenshot is missing the cloud surface handle");
        }
        if (name != "failure" && o.rotation_gizmo && o.gizmo_visible) {
            std::size_t colored{};
            for (unsigned axis=0;axis<3;++axis) {
                if(o.gizmo_axis>=0 && axis!=static_cast<unsigned>(o.gizmo_axis))continue;
                const auto& ring=o.rotation_gizmo->rings()[axis];
                for (std::size_t i = 0; i < ring.points.size(); ++i) {
                    if (!ring.projected[i] || !o.viewport.contains(ring.points[i])) continue;
                    const auto x = static_cast<u32>(ring.points[i].x * static_cast<f32>(screenshot.extent.width) / logical_size.x);
                    const auto y = static_cast<u32>(ring.points[i].y * static_cast<f32>(screenshot.extent.height) / logical_size.y);
                    if (x >= screenshot.extent.width || y >= screenshot.extent.height) continue;
                    const auto at = (std::size_t(y) * screenshot.extent.width + x) * 4;
                    const auto r = std::to_integer<unsigned>(screenshot.pixels[at]);
                    const auto g = std::to_integer<unsigned>(screenshot.pixels[at + 1]);
                    const auto b = std::to_integer<unsigned>(screenshot.pixels[at + 2]);
                    colored += (r > 180 && g < 160) || (g > 180 && r < 160) || (b > 180 && r < 160);
                }
            }
            require(colored >= 20, "Composed screenshot is missing rotation rings");
        }
        take(example::write_rgba8_png(directory_ / (std::string(name) + ".png"), screenshot.extent,
              {reinterpret_cast<const u8*>(screenshot.pixels.data()), screenshot.pixels.size()}));
        std::ofstream snapshot(directory_ / (std::string(name) + ".ui.txt"));
        snapshot << "frame=" << o.frame << " revision=" << o.state.document.revision << " image_revision=" << o.image_revision
                 << " selected=" << o.state.viewport.selected_object
                 << " inspected_mesh=" << static_cast<u32>(o.state.viewport.inspected_mesh)
                 << " gizmo=" << o.gizmo_visible << '\n';
        for (const auto& item : tree_.widgets)
            snapshot << item.id << " parent=" << item.parent << " role=" << static_cast<int>(item.role)
                     << " visible=" << item.visible << " enabled=" << item.enabled << " label=" << quoted(item.label)
                     << " text=" << quoted(item.text) << " bounds=" << item.bounds.x << ',' << item.bounds.y << ','
                     << item.bounds.width << ',' << item.bounds.height << " clip=" << item.clip.x << ',' << item.clip.y
                     << ',' << item.clip.width << ',' << item.clip.height << '\n';
        snapshot << "camera=" << o.state.viewport.editor_camera.yaw << ',' << o.state.viewport.editor_camera.pitch << ','
                 << o.state.viewport.editor_camera.distance << " time=" << o.state.viewport.time << '\n';
        for (const auto& instance : o.state.document.instances) {
            const auto p = position(o, instance.id);
            snapshot << "instance=" << instance.id << " blueprint=" << static_cast<u32>(instance.blueprint)
                     << " name=" << quoted(instance.name) << " evaluated_position=" << p.x << ',' << p.y << ',' << p.z << '\n';
        }
        snapshot << "status=" << o.status << "\nworker logs:\n" << o.worker_logs;
    }
    void report(std::string_view outcome) const {
        std::ofstream out(directory_ / "report.json");
        out << "{\n  \"outcome\": " << quoted(outcome) << ",\n  \"input_boundary\": \"Engine input events; actual UI layout/rendering and separate OpenGL worker; not browser or OS injection\",\n"
            << "  \"timing_scope\": \"Step predicate/action scheduling and auto-wait duration; queued input is applied on the next frame. Not end-to-end input latency or frame-time benchmarks. Separate frame-exact assertions verify immediate feedback.\",\n"
            << "  \"failure\": " << quoted(failure_) << ",\n  \"status\": " << quoted(last_status_) << ",\n  \"steps\": [\n";
        for (std::size_t i = 0; i < results_.size(); ++i)
            out << (i ? ",\n" : "") << "    {\"name\": " << quoted(results_[i].first) << ", \"milliseconds\": " << results_[i].second << '}';
        out << "\n  ]\n}\n";
        std::ofstream(directory_ / "worker.log") << last_logs_;
        std::ofstream gallery(directory_ / "index.html");
        gallery << "<!doctype html><meta charset=utf-8><title>VNG editor E2E</title>"
            "<style>body{margin:2rem;background:#10141b;color:#eee;font:16px system-ui}"
            "a{color:#83c9ff}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(480px,1fr));gap:24px}"
            "figure{margin:0;background:#1b2330;padding:14px}img{width:100%;height:auto}"
            "figcaption{padding:10px 0}table{border-collapse:collapse;width:100%}td,th{text-align:left;padding:6px;border-bottom:1px solid #334}"
            "pre{white-space:pre-wrap}</style><h1>Native editor E2E: " << html(outcome) << "</h1>"
            "<p>Real engine input, UI composition and separate OpenGL worker. No browser, OS-input injection, or direct scene mutation.</p>"
            "<p><a href=report.json>Machine-readable report</a> · <a href=worker.log>Worker log</a></p>";
        if (!failure_.empty()) gallery << "<pre>" << html(failure_) << "</pre>";
        std::vector<fs::path> images;
        for (const auto& entry : fs::directory_iterator(directory_))
            if (entry.path().extension() == ".png") images.push_back(entry.path().filename());
        std::ranges::sort(images);
        gallery << "<main>";
        for (const auto& path : images) {
            const auto filename = html(path.string()), stem = html(path.stem().string());
            gallery << "<figure><a href=\"" << filename << "\"><img loading=lazy src=\"" << filename
                    << "\" alt=\"" << stem << "\"></a><figcaption>" << stem << " · <a href=\"" << stem
                    << ".ui.txt\">Semantic UI / scene evidence</a></figcaption></figure>";
        }
        gallery << "</main><h2>Step timings</h2><p>Elapsed predicate/action-scheduling and auto-wait times include rendering waits and screenshot work. Queued input is applied on the next frame: these are not end-to-end input-latency, GPU or UI frame-time benchmarks. Separate frame-exact assertions verify immediate feedback.</p>"
                   "<table><tr><th>Step</th><th>Elapsed ms</th></tr>";
        for (const auto& [name, elapsed] : results_)
            gallery << "<tr><td>" << html(name) << "</td><td>" << elapsed << "</td></tr>";
        gallery << "</table>";
    }
    void select_viewport(std::string, std::function<u32()>);
    void choose_blueprint(std::string, std::function<project::BlueprintId()>, std::string screenshot = {});
    void drag(std::string name, bool cancel, f32 distance);
    void rotation_workflow();
    void forward_workflow();
    void attitude_workflow();
    void convenience_workflow();
    void scroll_workflow();
    void sidebar_resize_workflow();
    void fleet_timeline_workflow();
    void multiselect_workflow();
    void popout_workflow();
    void instance_modal_workflow();
    void box_drag(std::string name,input::Modifiers modifiers={},ui::Rect region={0,0,1,1});
    void workflow();
    void earth_workflow();
};

void Driver::earth_workflow() {
    const auto original=std::make_shared<content::vmesh::Document>();
    wait("Earth scene loaded without authoring a keyframe",[original](const Observation& o) {
        if(!ready(o))return false;
        require(!o.selected_keyframe,"Opening Earth selected a keyframe");
        const auto* mesh=project::mesh_geometry(o.state,static_cast<project::BlueprintId>(3));
        require(mesh && example::earth::is_earth(mesh->document()),"Earth scene lacks an Earth blueprint");
        *original=mesh->document();return true;
    });
    choose_blueprint("Earth",[]{return static_cast<project::BlueprintId>(3);});
    wait("Earth declares cloud controls in mesh edit without a selected keyframe",[this](const Observation& o) {
        if(!ready(o))return false;
        const auto* rebuild=find(button("Rebuild clouds"));
        if(!rebuild)return false;
        require(rebuild->enabled,"Blueprint controls incorrectly require scene keyframe selection");
        checkpoint(o,"earth-01-cloud-controls");return true;
    });
    const auto preview_revision=std::make_shared<u64>();
    add("Surface preview shortcut starts from the focused mesh viewport",[this,preview_revision](const Observation& o) {
        *preview_revision=o.state.document.revision;
        click_at(center(o.viewport));key(input::Key::four);return true;
    });
    for(const auto& [key_code,mode]:{std::pair{input::Key::one,project::MeshSelectMode::vertex},
            std::pair{input::Key::two,project::MeshSelectMode::edge},
            std::pair{input::Key::three,project::MeshSelectMode::face}}) {
        add("4 hides editor overlays without changing the Earth mesh",[this,preview_revision,key_code](const Observation& o) {
            require(o.mesh_tools && o.mesh_tools->mode()==project::MeshSelectMode::surface,"4 did not enter Surface mode");
            require(!o.mesh_overlay_visible,"Surface mode still displays component overlays");
            require(o.state.document.revision==*preview_revision,"Surface mode changed the document");
            key(key_code);return true;
        });
        add("Component shortcut restores its overlay",[this,mode](const Observation& o) {
            require(o.mesh_tools->mode()==mode && o.mesh_overlay_visible,"Component mode failed to restore its overlay");
            key(input::Key::four);return true;
        });
    }
    add("Earth surface is visible without editor wireframe",[this](const Observation& o) {
        require(!o.mesh_overlay_visible,"Surface preview still has an overlay");
        visible_pixels(o);checkpoint(o,"earth-01a-surface-preview");return true;
    });
    fill(field("Puff size","Earth clouds / mesh draft"),"1.4");
    fill(field("Coverage","Earth clouds / mesh draft"),"1.6");
    fill(field("Spiral size","Earth clouds / mesh draft"),"1.3");
    fill(field("Edge scatter","Earth clouds / mesh draft"),"1.25");
    fill(field("Height variation","Earth clouds / mesh draft"),"0.4");
    add("Slider drafts do not regenerate on every input",[original](const Observation& o) {
        require(o.state.document.mesh_drafts.empty(),"Changing a field regenerated the mesh before Rebuild");
        require(project::mesh_geometry(o.state,static_cast<project::BlueprintId>(3))->document()==*original,"Staged controls changed scene geometry");
        return true;
    });
    click(button("Rebuild clouds"));
    wait("Background rebuild reaches the preview as an unpublished draft",[this,original](const Observation& o) {
        if(!ready(o) || o.state.document.mesh_drafts.empty())return false;
        const auto& draft=project::editable_mesh(o.state)->document();
        const auto settings=take(example::earth::cloud_settings(draft));
        require(settings.puff_size==1.4F && settings.coverage==1.6F && settings.spiral_size==1.3F &&
            settings.edge_scatter==1.25F && settings.relief==.4F,"Cloud controls did not reach the generated mesh");
        require(project::mesh_geometry(o.state,static_cast<project::BlueprintId>(3))->document()==*original,"Rebuild prematurely published the Earth draft");
        visible_pixels(o);checkpoint(o,"earth-02-rebuilt-clouds");return true;
    });
    click(button("Undo"));
    wait("Undo rebuild restores the original blueprint",[](const Observation& o){return ready(o)&&o.state.document.mesh_drafts.empty();});
    click(button("Redo"));
    wait("Redo rebuild restores its settings and geometry",[](const Observation& o){return ready(o)&&!o.state.document.mesh_drafts.empty();});
    click(dropdown("Edit part"));click(option("Atlantic spiral","Edit part"));
    fill(field("Longitude (degrees)","Cloud formation / surface position"),"-15");
    fill(field("Latitude (degrees)","Cloud formation / surface position"),"5");
    click(button("Move cloud"));
    wait("Moving a whole formation changes only its unpublished surface placement",[this,original](const Observation& o) {
        if(!ready(o))return false;
        const auto& draft=project::editable_mesh(o.state)->document();
        const auto clouds=take(example::earth::cloud_formations(draft));
        if(std::abs(clouds[14].location.x+15)>.001F || std::abs(clouds[14].location.y-5)>.001F)return false;
        require(project::mesh_geometry(o.state,static_cast<project::BlueprintId>(3))->document()==*original,"Cloud move published a draft");
        require(o.state.document.instances.size()==1,"Cloud selection created a scene instance");
        visible_pixels(o);checkpoint(o,"earth-02a-moved-formation");return true;
    });
    click(button("Undo"));
    wait("Cloud move is one undoable operation",[](const Observation& o) {
        return ready(o)&&std::abs(take(example::earth::cloud_formations(project::editable_mesh(o.state)->document()))[14].location.x+46)<.001F;
    });
    click(button("Redo"));
    wait("Redo restores cloud position",[](const Observation& o) {
        return ready(o)&&std::abs(take(example::earth::cloud_formations(project::editable_mesh(o.state)->document()))[14].location.x+15)<.001F;
    });
    click(dropdown("Edit part"));click(option("Whole blueprint","Edit part"));
    add("Click actual visible cloud geometry rather than its list or handle",[this,preview_revision](const Observation& o) {
        const auto* mesh=project::editable_mesh(o.state);
        const auto& d=mesh->document();
        const auto field=std::ranges::find(d.vertex_fields,"earth/cloud",&content::vmesh::VertexField::name);
        require(field!=d.vertex_fields.end(),"Cloud ownership missing");
        const auto& ids=std::get<std::vector<u32>>(field->values);
        const auto camera=take(project::camera(o.state).snapshot(o.preview_extent));
        for(auto face:d.faces)if(ids[face[0]]==15) {
            Vec3 center{};for(auto id:face.vertices)for(unsigned c=0;c<3;++c)center[c]+=mesh->position(id)[c]/3;
            spatial::Ray3 ray;
            for(unsigned c=0;c<3;++c){ray.origin[c]=camera.position[c];ray.direction[c]=center[c]-camera.position[c];}
            const auto hit=mesh->picking_index().intersect(ray);
            if(!hit||ids[d.faces[hit->triangle][0]]!=15)continue;
            const auto pixel=project_point(o,center);if(!o.viewport.contains(pixel))continue;
            *preview_revision=o.state.document.revision;input_frame_=o.frame;
            click_at(pixel);return true;
        }
        throw std::runtime_error("No visible Atlantic cloud triangle to click");
    });
    add("Viewport selection immediately synchronizes cloud list and surface handle",[this,preview_revision](const Observation& o) {
        require(o.frame==input_frame_+1,"Cloud selection waited for another frame");
        const auto* choice=find(dropdown("Edit part"));
        require(choice&&choice->text=="Atlantic spiral","Viewport cloud click did not update the list");
        require(o.mesh_part_gizmo&&o.mesh_part_gizmo->handle()&&!o.dragging,"Cloud click did not immediately show a passive handle");
        require(o.state.document.revision==*preview_revision&&!o.blueprint_pending,"Cloud selection unnecessarily rebuilt geometry");
        checkpoint(o,"earth-02aa-picked-cloud");
        click_at({o.viewport.x+5,o.viewport.y+5});return true;
    });
    add("Empty space clears cloud selection without an authored edit",[this,preview_revision](const Observation& o) {
        const auto* choice=find(dropdown("Edit part"));
        require(choice&&choice->text=="Whole blueprint"&&!o.mesh_part_gizmo->visible(),"Background click failed to deselect cloud");
        require(o.state.document.revision==*preview_revision,"Deselect authored an edit");return true;
    });
    click(dropdown("Edit part"));click(option("Atlantic spiral","Edit part"));
    const auto handle_start=std::make_shared<Vec2>();
    const auto moved_location=std::make_shared<Vec2>();
    wait("Selected cloud exposes a viewport surface handle",[this,handle_start](const Observation& o) {
        if(!ready(o)||!o.mesh_part_gizmo||!o.mesh_part_gizmo->handle())return false;
        *handle_start=*o.mesh_part_gizmo->handle();
        checkpoint(o,"earth-02b-cloud-handle");
        pointer(input::EventKind::pointer_down,*handle_start);return true;
    });
    add("Cloud surface handle captures immediately",[this,handle_start](const Observation& o) {
        require(o.mesh_part_gizmo->dragging(),"Cloud handle failed to capture");
        pointer(input::EventKind::pointer_move,{handle_start->x+42,handle_start->y-18});
        input_frame_=o.frame;return true;
    });
    add("Handle follows the pointer before background geometry is ready",[this,handle_start](const Observation& o) {
        require(o.frame==input_frame_+1 && o.mesh_part_gizmo->dragging(),"Cloud handle response was delayed");
        const auto p=o.mesh_part_gizmo->handle();
        require(p && std::hypot(p->x-handle_start->x,p->y-handle_start->y)>5,"Cloud handle did not move locally");
        require(o.blueprint_pending,"Pending cloud job has no loading state");
        require(std::ranges::any_of(o.viewport_draw_list->commands,[](const auto& draw) {
            const auto* text=std::get_if<ui::TextDraw>(&draw);
            return text&&text->text=="Updating blueprint...";
        }),"Cloud update spinner/label missing");
        checkpoint(o,"earth-02c-cloud-pending");
        pointer(input::EventKind::pointer_move,{handle_start->x+58,handle_start->y-22});
        pointer(input::EventKind::pointer_up,{handle_start->x+58,handle_start->y-22});return true;
    });
    wait("Final cloud drag target reaches the actual worker image",[this,moved_location,original](const Observation& o) {
        if(!ready(o)||o.blueprint_pending||o.dragging)return false;
        *moved_location=take(example::earth::cloud_formations(project::editable_mesh(o.state)->document()))[14].location;
        require(std::abs(moved_location->x+15)>.01F || std::abs(moved_location->y-5)>.01F,"Drag did not change cloud placement");
        require(project::mesh_geometry(o.state,static_cast<project::BlueprintId>(3))->document()==*original,"Drag leaked into published geometry");
        checkpoint(o,"earth-02d-cloud-dragged");return true;
    });
    click(button("Undo"));
    wait("Whole cloud drag is one undo step",[](const Observation& o) {
        if(!ready(o)||o.blueprint_pending)return false;
        const auto p=take(example::earth::cloud_formations(project::editable_mesh(o.state)->document()))[14].location;
        return std::abs(p.x+15)<.001F&&std::abs(p.y-5)<.001F;
    });
    click(button("Redo"));
    wait("Redo restores the final coalesced cloud target",[moved_location](const Observation& o) {
        if(!ready(o)||o.blueprint_pending)return false;
        const auto p=take(example::earth::cloud_formations(project::editable_mesh(o.state)->document()))[14].location;
        return std::abs(p.x-moved_location->x)<.001F&&std::abs(p.y-moved_location->y)<.001F;
    });
    add("Begin another cloud gesture for cancellation",[this,handle_start](const Observation& o) {
        require(o.mesh_part_gizmo&&o.mesh_part_gizmo->handle(),"Cloud handle missing after Redo");
        *handle_start=*o.mesh_part_gizmo->handle();
        pointer(input::EventKind::pointer_down,*handle_start);return true;
    });
    add("Queue a new cloud target before cancel",[this,handle_start](const Observation& o) {
        require(o.mesh_part_gizmo->dragging(),"Cloud handle failed to recapture");
        pointer(input::EventKind::pointer_move,{handle_start->x+24,handle_start->y+12});return true;
    });
    add("RMB cancels an in-flight cloud job",[this](const Observation& o) {
        require(o.blueprint_pending,"Expected a pending cloud update");
        pointer(input::EventKind::pointer_down,pointer_,1);
        pointer(input::EventKind::pointer_up,pointer_,1);
        pointer(input::EventKind::pointer_up,pointer_);return true;
    });
    wait("Cancelled cloud job cannot reappear or open a mesh context menu",[this,moved_location](const Observation& o) {
        if(!ready(o)||o.blueprint_pending||o.dragging)return false;
        const auto p=take(example::earth::cloud_formations(project::editable_mesh(o.state)->document()))[14].location;
        require(std::abs(p.x-moved_location->x)<.001F&&std::abs(p.y-moved_location->y)<.001F,"Cancelled cloud job overwrote its starting pose");
        require(!find(button("Subdivide")),"Cancel RMB opened a mesh operation menu");
        return true;
    });
    const auto before_whole=std::make_shared<content::vmesh::Document>();
    add("Enter whole mesh mode without selecting any vertices",[this,before_whole](const Observation& o) {
        *before_whole=project::editable_mesh(o.state)->document();
        pointer(input::EventKind::pointer_move,{o.viewport.x+o.viewport.width*.65F,o.viewport.y+o.viewport.height*.5F});
        key(input::Key::five);return true;
    });
    const auto before_centered_navigation=std::make_shared<project::CameraPose>();
    add("5 enables whole mesh rotation and suppresses the cloud gizmo",[this,before_centered_navigation](const Observation& o) {
        require(o.mesh_tools->mode()==project::MeshSelectMode::whole&&o.mesh_tools->selected().empty(),"5 failed to target whole mesh");
        require(!o.mesh_overlay_visible&&!o.mesh_part_gizmo->visible(),"Whole mesh mode retained component or cloud handles");
        require(o.mesh_tools->transform_mode()==project::GizmoMode::rotate,"Whole mesh defaults to rotation");
        const auto* toggle=find({Role::checkbox,"Mesh-centered camera",{}});
        require(toggle&&toggle->bounds.x<o.viewport.x+30&&toggle->bounds.y<o.viewport.y+70,"Mesh camera toggle is not at the top left");
        *before_centered_navigation=project::view_camera(o.state);
        click_at(center(toggle->bounds));return true;
    });
    add("Mesh-centered camera toggles without moving the view or authoring geometry",[this,before_centered_navigation,before_whole](const Observation& o) {
        const auto* toggle=find({Role::checkbox,"Mesh-centered camera",{}});
        require(toggle&&toggle->checked.value_or(false),"Mesh-centered toggle did not turn on");
        require(project::view_camera(o.state)==*before_centered_navigation,"Toggle jumped the camera");
        require(project::editable_mesh(o.state)->document()==*before_whole,"Camera toggle authored mesh geometry");
        checkpoint(o,"earth-whole-mesh-navigation");
        pointer(input::EventKind::pointer_move,{o.viewport.x+o.viewport.width*.65F,o.viewport.y+o.viewport.height*.5F});
        key(input::Key::right,{.control=true});return true;
    });
    add("Ctrl Right cycles mesh capabilities rather than scene gizmos",[this](const Observation& o) {
        require(o.mesh_tools->transform_mode()==project::GizmoMode::scale,"Ctrl Right did not select mesh scale");
        key(input::Key::left,{.control=true});return true;
    });
    add("Ctrl Left returns to mesh rotation",[this](const Observation& o) {
        require(o.mesh_tools->transform_mode()==project::GizmoMode::rotate,"Ctrl Left did not select mesh rotation");
        key(input::Key::r);key(input::Key::y);return true;
    });
    add("Whole mesh R Y captures with empty component selection",[this](const Observation& o) {
        require(o.dragging,"Whole mesh rotation required a selection");
        pointer(input::EventKind::pointer_move,{pointer_.x+65,pointer_.y-50});return true;
    });
    const auto held_placement=std::make_shared<Mat4>();
    const auto held_camera=std::make_shared<project::CameraPose>();
    for(auto modifiers:{input::Modifiers{.control=true},input::Modifiers{.shift=true},input::Modifiers{}}) {
        add("Navigate camera while whole mesh rotation stays active",[this,held_placement,held_camera,modifiers](const Observation& o) {
            require(o.dragging,"Transform disappeared before camera navigation");
            require(project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true)!=Mat4::identity(),"Camera toggle focus blocked mesh rotation");
            *held_placement=project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true);
            *held_camera=project::view_camera(o.state);
            pointer(input::EventKind::pointer_down,pointer_,2,modifiers);return true;
        });
        add("Drag and release camera without transforming the blueprint",[this,modifiers](const Observation& o) {
            require(o.dragging,"Camera drag cancelled the gizmo");
            const Vec2 end{pointer_.x+20,pointer_.y+12};
            pointer(input::EventKind::pointer_move,end,2,modifiers);
            pointer(input::EventKind::pointer_up,end,2,modifiers);return true;
        });
        wait("Camera preview updates while transform and baseline survive",[this,held_placement,held_camera](const Observation& o) {
            if(!ready(o))return false;
            require(o.dragging,"Camera movement ended the active transform");
            require(project::view_camera(o.state)!=*held_camera,"Camera remained blocked by gizmo capture");
            require(project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true)==*held_placement,"Camera navigation also transformed the mesh");
            pointer(input::EventKind::pointer_move,pointer_);return true;
        });
        add("Resuming at the same pointer does not jump",[held_placement](const Observation& o) {
            require(project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true)==*held_placement,"Resuming the gizmo caused a jump");return true;
        });
    }
    add("Start holding a rotation arrow",[this,held_placement](const Observation& o) {
        *held_placement=project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true);
        events_.push_back({.kind=input::EventKind::key_down,.position=pointer_,.key=input::Key::up});return true;
    });
    for(int frame=0;frame<8;++frame)add("Held arrow advances whole-mesh rotation without repeat events",[held_placement](const Observation& o) {
        const auto placement=project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true);
        require(o.dragging,"Held arrow ended the transform");
        require(placement!=*held_placement,"Held arrow stalled before keyboard repeat");
        *held_placement=placement;return true;
    });
    add("Release held arrow",[this](const Observation&) {
        events_.push_back({.kind=input::EventKind::key_up,.position=pointer_,.key=input::Key::up});return true;
    });
    add("Record released rotation",[held_placement](const Observation& o) {
        *held_placement=project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true);return true;
    });
    add("No held motion leaks after key release",[held_placement](const Observation& o) {
        require(project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true)==*held_placement,"Arrow kept rotating after release");return true;
    });
    const auto resume_pointer=std::make_shared<Vec2>();
    add("Active gizmo offers a bottom-left sensitivity control",[this,held_placement,resume_pointer](const Observation& o) {
        const auto* slider=find({ui::WidgetRole::slider,"Sensitivity"});
        require(slider,"Sensitivity slider is missing");
        require(slider->bounds.x<o.viewport.x+o.viewport.width*.5F,"Gizmo menu is not on the left");
        *resume_pointer=pointer_;
        *held_placement=project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true);
        checkpoint(o,"earth-gizmo-navigation-options");
        const auto r=slider->bounds;click_at({r.x+r.width*.6F,r.y+r.height*.5F});return true;
    });
    add("Sensitivity editing does not move or confirm the transform",[this,held_placement,resume_pointer](const Observation& o) {
        require(o.dragging,"Sensitivity click confirmed the modal gizmo");
        const auto* slider=find({ui::WidgetRole::slider,"Sensitivity"});
        require(slider&&slider->number&&*slider->number>1,"Sensitivity did not change");
        require(project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true)==*held_placement,"Slider interaction transformed the mesh");
        pointer(input::EventKind::pointer_move,*resume_pointer);return true;
    });
    add("Return from tool options keeps the transform stable",[this,held_placement](const Observation& o) {
        require(o.dragging,"Tool options ended the gizmo");
        require(project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true)==*held_placement,"Returning from options caused a jump");
        key(input::Key::enter);return true;
    });
    wait("Whole mesh rotation reaches the preview without publishing",[this,before_whole,original](const Observation& o) {
        if(!ready(o)||o.dragging)return false;
        require(project::editable_mesh(o.state)->document()==*before_whole,"Whole rotation rewrote vertex storage");
        require(project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true)!=Mat4::identity(),"Whole rotation changed no placement");
        require(project::mesh_geometry(o.state,static_cast<project::BlueprintId>(3))->document()==*original,"Whole rotation published its draft");
        checkpoint(o,"earth-02e-whole-rotated");
        key(input::Key::s);return true;
    });
    add("Whole mesh scale captures without selecting components",[this](const Observation& o) {
        require(o.dragging&&o.mesh_tools->selected().empty(),"Whole mesh scale required selected vertices");
        pointer(input::EventKind::pointer_move,{pointer_.x+45,pointer_.y});key(input::Key::enter);return true;
    });
    wait("Whole mesh scale reaches the preview",[this](const Observation& o) {
        if(!ready(o)||o.dragging)return false;
        checkpoint(o,"earth-02f-whole-scaled");return true;
    });
    click(button("Undo"));wait("Undo whole scale",[](const Observation& o){return ready(o);});
    click(button("Undo"));wait("Undo whole rotation restores exact geometry and cloud recipe",[before_whole](const Observation& o) {
        if(!ready(o))return false;
        require(o.state.document.mesh_placements.empty(),"Undo left a mesh placement behind");
        require(project::editable_mesh(o.state)->document()==*before_whole,"Whole transforms were not separate exact undo steps");return true;
    });
    click(button("Bake camera transforms..."));
    add("Camera bake exposes independent rotation and scale choices",[this](const Observation& o) {
        require(find({Role::checkbox,"Rotation",{}})&&find({Role::checkbox,"Scale",{}}),"Camera bake options missing");
        checkpoint(o,"earth-camera-bake-menu");return true;
    });
    click(button("Bake to mesh draft"));
    wait("Camera view becomes an unpublished mesh placement",[this,before_whole,original](const Observation& o) {
        if(!ready(o))return false;
        require(project::view_camera(o.state).yaw==project::CameraPose{}.yaw&&project::view_camera(o.state).pitch==project::CameraPose{}.pitch,"Baked camera rotation was not reset");
        require(project::mesh_placement(o.state,static_cast<project::BlueprintId>(3),true)!=Mat4::identity(),"Camera bake changed no mesh placement");
        require(project::editable_mesh(o.state)->document()==*before_whole,"Camera bake rebuilt vertex buffers");
        require(project::mesh_geometry(o.state,static_cast<project::BlueprintId>(3))->document()==*original,"Camera bake published without Apply");
        checkpoint(o,"earth-camera-baked");return true;
    });
    click(button("Undo"));wait("Undo camera bake restores mesh placement in one step",[](const Observation& o) {
        return ready(o)&&o.state.document.mesh_placements.empty();
    });
    // Choosing a formation from the list leaves whole-mesh mode automatically.
    click(dropdown("Edit part"));click(option("Atlantic spiral","Edit part"));
    wait("List part selection restores the surface gizmo after whole mesh mode",[this](const Observation& o) {
        if(!ready(o)||o.mesh_tools->mode()!=project::MeshSelectMode::surface||!o.mesh_part_gizmo->visible())return false;
        require(find({Role::checkbox,"Mesh-centered camera",{}}),"Surface mode lost mesh-centered navigation");
        require(!find(button("Bake camera transforms...")),"Camera bake leaked outside whole mesh mode");return true;
    });
    const auto before_heading=std::make_shared<content::vmesh::Document>();
    add("Cloud heading ring accepts R and arrow nudges",[this,before_heading](const Observation& o) {
        *before_heading=project::editable_mesh(o.state)->document();
        require(o.mesh_part_gizmo->rotation().visible(),"Cloud heading ring missing");
        const auto p=*o.mesh_part_gizmo->handle();
        pointer(input::EventKind::pointer_move,{p.x+90,p.y});key(input::Key::r);return true;
    });
    add("Nudge the cloud heading and confirm",[this](const Observation& o) {
        require(o.dragging,"R did not capture the cloud heading");
        for(int i=0;i<8;++i)key(input::Key::up);
        key(input::Key::enter);return true;
    });
    wait("Cloud heading reaches the actual worker image",[this,before_heading](const Observation& o) {
        if(!ready(o)||o.dragging||o.blueprint_pending)return false;
        require(project::editable_mesh(o.state)->document()!=*before_heading,"Cloud arrow rotation changed nothing");
        checkpoint(o,"earth-02g-cloud-heading");return true;
    });
    click(button("Undo"));
    wait("Undo restores the full cloud heading recipe",[before_heading](const Observation& o) {
        return ready(o)&&!o.blueprint_pending&&project::editable_mesh(o.state)->document()==*before_heading;
    });
    click(button("Add cloud bank"));
    wait("Adding a formation selects its new blueprint-local identity",[this](const Observation& o) {
        if(!ready(o)||o.blueprint_pending)return false;
        const auto clouds=take(example::earth::cloud_formations(project::editable_mesh(o.state)->document()));
        require(clouds.size()==18,"Cloud creation did not add one formation");
        const auto* part=find(dropdown("Edit part"));require(part&&part->text==clouds.back().name,"New cloud was not selected");
        checkpoint(o,"earth-02h-added-cloud");return true;
    });
    click(button("Remove cloud"));
    wait("Removing a cloud restores the formation count",[](const Observation& o) {
        return ready(o)&&!o.blueprint_pending&&take(example::earth::cloud_formations(project::editable_mesh(o.state)->document())).size()==17;
    });
    click(dropdown("Edit part"));click(option("Whole blueprint","Edit part"));
    // The short properties panel scrolls naturally to the ordinary mesh actions.
    add("Scroll to mesh publish action",[this](const Observation&) {
        const auto* rebuild=find(button("Rebuild clouds"));require(rebuild,"Cloud panel missing");
        pointer_=center(rebuild->bounds);
        events_.push_back({.kind=input::EventKind::scroll,.position=pointer_,.scroll={0,-10}});return true;
    });
    click(button("Apply mesh to scene"));
    wait("Apply publishes cloud geometry to scene instances",[this,original](const Observation& o) {
        if(!ready(o) || !o.state.document.mesh_drafts.empty())return false;
        const auto& mesh=project::mesh_geometry(o.state,static_cast<project::BlueprintId>(3))->document();
        require(mesh!=*original && take(example::earth::cloud_settings(mesh)).puff_size==1.4F,"Apply did not publish regenerated clouds");
        checkpoint(o,"earth-03-applied-clouds");return true;
    });
    click(dropdown("View"));click(option("Scene","View"));
    wait("Published cloud geometry renders through the scene's normal renderer",[this](const Observation& o) {
        if(!ready(o) || o.state.viewport.mode!=project::ViewMode::scene)return false;
        visible_pixels(o);checkpoint(o,"earth-04-scene-clouds");return true;
    });
    click(button("Undo"));
    wait("Undo Apply restores published terrain plus the retained cloud draft",[original](const Observation& o) {
        return ready(o) && !o.state.document.mesh_drafts.empty() &&
            project::mesh_geometry(o.state,static_cast<project::BlueprintId>(3))->document()==*original;
    });
}

void Driver::instance_modal_workflow() {
    click(button("Scene")); // No text field owns the transform keys.
    for(auto operation:{input::Key::g,input::Key::r,input::Key::s})for(auto axis:{input::Key::unknown,input::Key::x,input::Key::y,input::Key::z}) {
        auto original=std::make_shared<std::vector<project::SceneInstance>>();
        auto timeline=std::make_shared<timeline::Timeline>();
        auto selected=std::make_shared<std::vector<u32>>();
        add("Start instance keyboard transform",[this,original,timeline,selected,operation,axis](const Observation& o) {
            if(!ready(o))return false;
            *original=o.state.document.instances;*timeline=o.state.document.timeline;
            selected->assign(o.selected_instances.begin(),o.selected_instances.end());
            require(!selected->empty(),"No selected instances for G/R/S test");
            viewport_events_=o.viewport_detached;
            const auto p=center(o.viewport);
            pointer(input::EventKind::pointer_move,{p.x+100,p.y});key(operation);
            if(axis!=input::Key::unknown)key(axis);
            return true;
        });
        add("Instance keyboard transform captures with no mouse button",[this,operation,axis](const Observation& o) {
            require(o.dragging,"Instance G/R/S did not capture");
            require(o.gizmo_visible,"Keyboard transform hid the active gizmo");
            if(operation==input::Key::r && axis==input::Key::x)checkpoint(o,"modal-rotate-x-"+std::to_string(o.frame));
            pointer(input::EventKind::pointer_move,{pointer_.x+55,pointer_.y-45});return true;
        });
        add("Instance keyboard transform changes local state immediately",[this,original,timeline,selected,operation](const Observation& o) {
            require(o.state.document.instances!=*original || o.state.document.timeline!=*timeline,"G/R/S did not update authored values");
            require(std::ranges::equal(o.selected_instances,*selected),"Transform altered selection");
            if(operation==input::Key::s)click_at(pointer_);else key(input::Key::enter);
            return true;
        });
        wait("Instance keyboard transform commits without selecting behind it",[this,selected](const Observation& o) {
            if(!ready(o)||o.dragging)return false;
            require(std::ranges::equal(o.selected_instances,*selected),"Confirm click changed selection");
            viewport_events_=false;return true;
        });
        click(button("Undo"));
        wait("Instance keyboard transform undoes as one transaction",[original,timeline](const Observation& o) {
            if(!ready(o))return false;
            require(o.state.document.instances==*original && o.state.document.timeline==*timeline,"Undo did not restore the entire transform");
            return true;
        });
    }
    add("Begin cancelled keyboard move",[this](const Observation& o) {
        group_before_=o.state.document.instances;
        viewport_events_=o.viewport_detached;
        const auto p=center(o.viewport);pointer(input::EventKind::pointer_move,p);key(input::Key::g);return true;
    });
    add("Move before RMB cancellation",[this](const Observation& o) {
        require(o.dragging,"Move failed to capture");
        pointer(input::EventKind::pointer_move,{pointer_.x+40,pointer_.y+20});return true;
    });
    add("RMB cancels instead of opening a region menu",[this](const Observation& o) {
        require(o.state.document.instances!=group_before_,"Cancel test did not move the instance");
        pointer(input::EventKind::pointer_down,pointer_,1);pointer(input::EventKind::pointer_up,pointer_,1);return true;
    });
    wait("Cancelled modal move restores all instances",[this](const Observation& o) {
        require(!o.modal,"Cancelling a transform opened a region menu");
        if(!ready(o)||o.dragging)return false;
        require(o.state.document.instances==group_before_,"RMB did not roll back instance transform");
        viewport_events_=false;return true;
    });
    click(dropdown("Gizmo"));click(option("Move","Gizmo"));
}

void Driver::popout_workflow() {
    wait("Initial viewport",[](const Observation& o){return ready(o);});
    click(button("0 s | Keyframe"));
    click({Role::checkbox,"FPS",{}});
    click(button("Pop out viewport"));
    wait("Detached viewport has its own native pixels",[this](const Observation& o) {
        if (!o.viewport_detached || !ready(o)) return false;
        require(o.viewport_draw_list && o.viewport_draw_list->framebuffer==Extent2D{1200,900},"Wrong popout drawable");
        require(fps_text(*o.viewport_draw_list) && !fps_text(o.draw_list),
                "FPS overlay did not move exclusively to the detached viewport");
        require(o.preview_extent.width>900,"Detached preview did not resize");
        require(std::ranges::none_of(o.draw_list.commands,[&](const auto& c) {
            const auto* image=std::get_if<ui::ImageDraw>(&c); return image && image->pixels.get()==o.preview_pixels;
        }),"Controls window still contains the viewport image");
        checkpoint(o,"popout-01-detached");
        viewport_events_=true; return true;
    });
    select_viewport("detached cube",[]{return 1U;});
    instance_modal_workflow();
    add("Return keyboard focus to detached viewport",[this](const Observation&){viewport_events_=true;return true;});
    add("Prepare detached gizmo drag",[this](const Observation&){imported_=1;return true;});
    drag("Detached translation",true,50);
    add("Wheel in detached viewport uses default forward motion",[this](const Observation& o) {
        if (!ready(o)) return false;
        zoom_before_=o.state.viewport.editor_camera; zoom_revision_=o.state.document.revision;
        events_.push_back({.kind=input::EventKind::scroll,.position=center(o.viewport),.scroll={0,.2F}});
        return true;
    });
    wait("Detached navigation settles without changing the document",[this](const Observation& o) {
        if (!ready(o)) return false;
        require(o.state.viewport.editor_camera.target!=zoom_before_.target,"Wheel did not move detached camera");
        require(o.state.viewport.editor_camera.zoom==zoom_before_.zoom,"Wheel changed lens zoom");
        require(o.state.document.revision==zoom_revision_ && !o.dirty,"Navigation dirtied document");
        checkpoint(o,"popout-02-selection-navigation");
        key(input::Key::c,{.control=true}); return true;
    });
    add("Copy shortcut reaches shared editor session",[this](const Observation& o) {
        require(o.status.starts_with("Copied"),"Detached copy failed");
        key(input::Key::v,{.control=true}); return true;
    });
    wait("Paste in detached viewport creates an editable instance",[this](const Observation& o) {
        if (!ready(o)) return false;
        require(o.state.document.instances.size()==3 && o.gizmo_visible,"Detached paste failed");
        key(input::Key::z,{.control=true}); return true;
    });
    wait("Detached undo restores scene",[](const Observation& o){return ready(o)&&o.state.document.instances.size()==2;});
    click(button("Play (independent)"));
    wait("Independent Play coexists with detached viewport",[this](const Observation& o) {
        if (!o.playing || !ready(o)) return false;
        require(o.viewport_detached,"Independent Play docked the viewport");
        checkpoint(o,"popout-03-independent-play"); return true;
    });
    click(button("Stop (independent)"));
    wait("Return to detached editing",[](const Observation& o){return !o.playing && ready(o) && o.viewport_detached;});
    choose_blueprint("detached cube",[]{return project::BlueprintId::mesh;});
    add("Open mesh context menu in detached window",[this](const Observation& o) {
        if (!ready(o)) return false;
        viewport_events_=true;
        pointer(input::EventKind::pointer_down,center(o.viewport),1);
        pointer(input::EventKind::pointer_up,center(o.viewport),1);
        return true;
    });
    wait("Detached mesh menu is interactive",[this](const Observation& o) {
        if (!o.modal || !find(button("Subdivide"))) return false;
        require(o.mesh_overlay_visible,"Detached RMB menu hid mesh wireframe and vertices");
        checkpoint(o,"popout-04-mesh-menu");
        key(input::Key::escape); return true;
    });
    wait("Escape closes only the viewport menu",[](const Observation& o){return !o.modal && ready(o);});
    add("Start a mesh gizmo with its own options in the detached viewport", [this](const Observation&) {
        viewport_events_=true;
        key(input::Key::g);
        return true;
    });
    wait("Gizmo options expose cancellation without hiding mesh overlays", [this](const Observation& o) {
        const auto* cancel=find(button("Cancel transform"));
        if (!o.dragging || !cancel) return false;
        require(o.mesh_overlay_visible,"Gizmo options hid the mesh overlay");
        viewport_events_=true;
        pointer(input::EventKind::pointer_move,center(cancel->bounds));
        key(input::Key::escape);
        return true;
    });
    wait("Escape still cancels a gizmo while hovering its options", [this](const Observation& o) {
        return !o.dragging && !find(button("Cancel transform")) && ready(o);
    });
    click(button("Dock viewport"));
    wait("Docking retains scene and camera",[this](const Observation& o) {
        if (o.viewport_detached || !ready(o)) return false;
        require(o.state.document.instances.size()==2,"Docking changed the scene");
        require(fps_text(o.draw_list),"FPS overlay was lost when docking the viewport");
        checkpoint(o,"popout-04-docked"); return true;
    });
    // Repeated creation/destruction exercises GL context ownership as well as
    // destruction while still detached on application exit.
    click(button("Pop out viewport"));
    wait("Reopen editable viewport",[](const Observation& o){return o.viewport_detached && ready(o);});
}

void Driver::box_drag(std::string name,input::Modifiers modifiers,ui::Rect region) {
    add(name+": press",[this,modifiers,region](const Observation& o) {
        if(!ready(o)) return false;
        drag_start_={o.viewport.x+region.x*o.viewport.width+6,o.viewport.y+region.y*o.viewport.height+6};
        drag_direction_={o.viewport.x+(region.x+region.width)*o.viewport.width-6,
            o.viewport.y+(region.y+region.height)*o.viewport.height-6};
        events_.push_back({.kind=input::EventKind::pointer_down,.position=drag_start_,.modifiers=modifiers});
        return true;
    });
    add(name+": drag",[this](const Observation&) {
        pointer(input::EventKind::pointer_move,drag_direction_);return true;
    });
    add(name+": rectangle visible",[this,name](const Observation& o) {
        require(o.box_selecting,"LMB drag did not display selection rectangle");
        checkpoint(o,name+"-rectangle");
        pointer(input::EventKind::pointer_up,drag_direction_);return true;
    });
}

void Driver::multiselect_workflow() {
    wait("Multi-selection fixture ready",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.state.document.instances.size()==2,"Wrong multi-selection fixture");
        group_before_=o.state.document.instances;fleet_revision_=o.state.document.revision;
        imported_=2;
        key(input::Key::v,{.control=true}); // Empty authoring clipboard.
        return true;
    });
    add("Failed paste reports failure without claiming success", [this](const Observation& o) {
        require(o.status.starts_with("Paste failed:"), "Failed paste showed no failure feedback");
        require(o.state.document.instances == group_before_ && o.state.document.revision == fleet_revision_ && !o.dirty,
                "Failed paste changed the document");
        return true;
    });
    click(button("0 s | Keyframe"));
    box_drag("multi-01-instances");
    wait("Box selects both instances without document edits",[this](const Observation& o) {
        if(!ready(o) || !o.inspector_ready) return false;
        require(o.selected_instances.size()==2,"Box did not select both instance origins");
        require(o.status.starts_with("Selected 2 instances"), "Selection feedback missed the actual count");
        require(o.state.document.revision==fleet_revision_ && !o.dirty,"Selection changed scene content");
        checkpoint(o,"multi-02-selected-instances");return true;
    });
    instance_modal_workflow();
    drag("Move selected group",false,18);
    wait("Group translation preserves offsets and shares no blueprint writes",[this](const Observation& o) {
        if(!ready(o)) return false;
        const auto a=position(o,1),b=position(o,2);
        const auto original_a=group_before_[0].transform.position,original_b=group_before_[1].transform.position;
        for(std::size_t i=0;i<3;++i)
            require(std::abs((a[i]-original_a[i])-(b[i]-original_b[i]))<1e-4F,"Only active instance moved");
        checkpoint(o,"multi-03-group-moved");key(input::Key::z,{.control=true});return true;
    });
    wait("Group move undo is one transaction",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.state.document.instances==group_before_,"Undo did not restore entire group");
        key(input::Key::c,{.control=true});return true;
    });
    add("Paste selected instances",[this](const Observation& o) {
        require(o.status.starts_with("Copied 2 instances"), "Successful copy did not report its count");
        key(input::Key::v,{.control=true});return true;
    });
    wait("Paste selects all copies",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.state.document.instances.size()==4 && o.selected_instances.size()==2,"Group clipboard lost instances or selection");
        require(std::ranges::all_of(o.selected_instances,[](u32 id){return id>2;}),"Paste selected originals");
        require(o.status.starts_with("Pasted 2 instances in place"), "Successful paste did not report its count");
        checkpoint(o,"multi-copy-paste-feedback");
        key(input::Key::del);return true;
    });
    wait("Delete removes only selected copies and retains blueprints",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.state.document.instances==group_before_ && project::blueprint_catalog(o.state).size()==3,"Batch delete damaged originals or blueprints");
        key(input::Key::z,{.control=true});return true;
    });
    wait("Undo restores deleted group",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.state.document.instances.size()==4,"Undo restored only one deleted instance");
        key(input::Key::z,{.control=true});return true; // Undo paste too.
    });
    wait("Original scene restored",[this](const Observation& o) {return ready(o)&&o.state.document.instances.size()==2;});
    box_drag("multi-fast-copy-paste");
    add("Queue copy and two pastes in one input batch", [this](const Observation& o) {
        require(o.selected_instances.size() == 2, "Fast shortcut test needs both instances selected");
        input_frame_ = o.frame;
        key(input::Key::c, {.control=true});
        key(input::Key::v, {.control=true});
        key(input::Key::v, {.control=true});
        return true;
    });
    add("Every shortcut in the burst executes immediately", [this](const Observation& o) {
        require(o.frame == input_frame_ + 1 && o.state.document.instances.size() == 6,
                "Copy/paste burst lost or delayed a shortcut");
        require(o.selected_instances.size() == 2 && o.gizmo_visible,
                "Fast paste did not select its copies and show a gizmo");
        require(o.status.starts_with("Pasted 2 instances in place"), "Fast paste missed success feedback");
        key(input::Key::z, {.control=true});
        key(input::Key::z, {.control=true});
        return true;
    });
    wait("Batched undo restores both pastes", [this](const Observation& o) {
        return ready(o) && o.state.document.instances == group_before_;
    });
    box_drag("multi-partial-left",{},{0,0,.5F,1});
    wait("Partial rectangle excludes the cube",[](const Observation& o) {
        if(!ready(o))return false;
        require(o.selected_instances.size()==1&&o.selected_instances[0]==2,"Partial box selected an outside instance");return true;
    });
    box_drag("multi-partial-add-right",{.shift=true},{.5F,0,.5F,1});
    wait("Add rectangle retains the first selection",[](const Observation& o) {
        if(!ready(o))return false;require(o.selected_instances.size()==2,"Shift-box replaced existing selection");return true;
    });
    const auto select_instance=[this](u32 id,input::Modifiers modifiers) {
        add("Modifier-click instance "+std::to_string(id),[this,id,modifiers](const Observation& o) {
            if(!ready(o)) return false;
            const auto* instance=project::find_instance(o.state,id);
            const auto label=(o.state.viewport.selected_object==id?"> #":std::ranges::find(o.selected_instances,id)!=o.selected_instances.end()?"+ #":"#")+
                std::to_string(id)+" "+instance->name;
            const auto* target=actionable(button(label));if(!target) return false;
            const auto p=center(intersection(target->bounds,target->clip));
            events_.push_back({.kind=input::EventKind::pointer_down,.position=p,.modifiers=modifiers});
            events_.push_back({.kind=input::EventKind::pointer_up,.position=p,.modifiers=modifiers});return true;
        });
    };
    select_instance(1,{});select_instance(2,{.shift=true});
    wait("Instance Shift-click selects range",[](const Observation& o){if(!ready(o))return false;require(o.selected_instances.size()==2,"Instance range selection failed");return true;});
    select_instance(1,{.control=true});
    wait("Instance Ctrl-click toggles one",[](const Observation& o){if(!ready(o))return false;require(o.selected_instances.size()==1&&o.selected_instances[0]==2,"Instance toggle failed");return true;});
    box_drag("multi-04-subtract",{.control=true});
    wait("Ctrl-box removes selection",[this](const Observation& o) {
        if(!ready(o))return false;
        require(o.selected_instances.empty(),"Ctrl-box did not subtract");
        key(input::Key::c, {.control=true});
        return true;
    });
    add("Failed copy does not claim instances were copied", [this](const Observation& o) {
        require(o.status.starts_with("Copy failed:"), "Empty selection reported a successful copy");
        require(o.state.document.instances == group_before_, "Failed copy changed the document");
        return true;
    });
    box_drag("multi-05-add",{.shift=true});
    wait("Shift-box adds both",[](const Observation& o){if(!ready(o))return false;require(o.selected_instances.size()==2,"Shift-box did not add");return true;});

    fill(field("Time"),"3");add("Seek first timestamp",[this](const Observation&){key(input::Key::enter);return true;});
    click(button("Add keyframe"));
    fill(field("Time"),"5");add("Seek second timestamp",[this](const Observation&){key(input::Key::enter);return true;});
    click(button("Add keyframe"));
    const auto select_key=[this](f32 time,input::Modifiers modifiers) {
        add("Modifier-click keyframe "+std::to_string(time),[this,time,modifiers](const Observation& o) {
            if(!ready(o))return false;
            const auto selected=std::ranges::find(o.selected_keyframes,time)!=o.selected_keyframes.end();
            const auto label=(o.selected_keyframe==time?"> ":selected?"+ ":"")+std::to_string(static_cast<int>(time))+" s | Keyframe";
            const auto* target=actionable(button(label));if(!target)return false;
            const auto p=center(intersection(target->bounds,target->clip));
            events_.push_back({.kind=input::EventKind::pointer_down,.position=p,.modifiers=modifiers});
            events_.push_back({.kind=input::EventKind::pointer_up,.position=p,.modifiers=modifiers});return true;
        });
    };
    select_key(0,{});select_key(5,{.shift=true});
    wait("Keyframe range selects timestamps",[this](const Observation& o){if(!ready(o))return false;require(o.selected_keyframes.size()==3,"Keyframe range missed timestamp");checkpoint(o,"multi-06-keyframe-range");return true;});
    select_key(3,{.control=true});
    wait("Keyframe Ctrl toggle preserves others",[this](const Observation& o){if(!ready(o))return false;require(o.selected_keyframes.size()==2,"Keyframe toggle failed");key(input::Key::del);return true;});
    wait("Delete preserves initial pose",[this](const Observation& o){if(!ready(o))return false;require(project::keyframe_times(o.state)==std::vector<f32>{0,3},"Batch keyframe delete did not preserve only zero and the unselected key");key(input::Key::z,{.control=true});return true;});
    wait("Undo restores all keyframes",[](const Observation& o){return ready(o)&&project::keyframe_times(o.state).size()==3;});
    add("Redo batch keyframe deletion", [this](const Observation&) {
        key(input::Key::y, {.control=true}); return true;
    });
    wait("Redo removes the same timestamps", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(project::keyframe_times(o.state)==std::vector<f32>{0,3}, "Redo lost batch deletion");
        key(input::Key::z, {.control=true}); return true;
    });
    wait("Undo restores timestamps for marker selection", [](const Observation& o) {
        return ready(o) && project::keyframe_times(o.state).size()==3;
    });
    const auto select_marker = [this](f32 time, input::Modifiers modifiers) {
        add("Modifier-click timeline marker " + std::to_string(time),
            [this,time,modifiers](const Observation& o) {
                if (!ready(o)) return false;
                const auto* heading=find({Role::label,"TIMELINE",{}});
                require(heading!=nullptr, "Timeline heading missing");
                const auto bar=std::ranges::find(tree_.widgets,heading->parent,&ui::WidgetSnapshot::id);
                require(bar!=tree_.widgets.end(), "Timeline toolbar missing");
                const auto canvas=std::ranges::find_if(tree_.widgets,[&](const auto& widget) {
                    return widget.parent==bar->parent && widget.role==Role::image && widget.visible;
                });
                require(canvas!=tree_.widgets.end(), "Timeline canvas missing");
                const Vec2 point{canvas->bounds.x + canvas->bounds.width*time/o.state.document.timeline_duration,
                                 canvas->bounds.y + 40};
                events_.push_back({.kind=input::EventKind::pointer_down,.position=point,.modifiers=modifiers});
                events_.push_back({.kind=input::EventKind::pointer_up,.position=point,.modifiers=modifiers});
                return true;
            });
    };
    select_marker(0, {});
    select_marker(5, {.shift=true});
    click(button("Scene"));
    wait("Timeline marker range shares list selection", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(o.selected_keyframes.size()==3, "Marker Shift-click did not select range");
        require(find({Role::label,"KEYFRAMES / 3 selected",{}},false)!=nullptr,
                "Selected count missing from keyframe list");
        checkpoint(o,"multi-06b-marker-range"); return true;
    });
    select_marker(3, {.control=true});
    wait("Timeline marker toggle preserves other keys", [](const Observation& o) {
        if (!ready(o)) return false;
        require(o.selected_keyframes.size()==2 &&
                std::ranges::find(o.selected_keyframes,3.F)==o.selected_keyframes.end(),
                "Marker Ctrl-click did not toggle timestamp");
        return true;
    });
    click(button("Delete selected keys"));
    wait("Inspector button deletes marker-selected batch", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(project::keyframe_times(o.state)==std::vector<f32>{0,3}, "Delete button did not preserve zero while removing selected batch");
        key(input::Key::z, {.control=true}); return true;
    });
    wait("Undo restores marker-selected batch", [](const Observation& o) {
        return ready(o) && project::keyframe_times(o.state).size()==3;
    });
    choose_blueprint("box selection cube",[]{return project::BlueprintId::mesh;});
    box_drag("multi-07-mesh");
    wait("Mesh box selects visible vertices without editing geometry",[this](const Observation& o) {
        if(!ready(o))return false;
        require(o.mesh_tools && o.mesh_tools->selected().size()>1,"Mesh box failed");
        require(o.state.document.mesh_drafts.empty(),"Selection created a mesh draft");
        checkpoint(o,"multi-08-mesh-selected");return true;
    });
}

void Driver::fleet_timeline_workflow() {
    wait("Asteroid fleet scene is ready", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(o.state.document.instances.size() == example::asteroids::rock_count+example::asteroids::fleet_count+1, "Wrong fleet fixture");
        std::size_t oriented{}, unoriented{};
        for (const auto& source : o.state.document.instances) {
            const auto* mesh=project::mesh_geometry(o.state,source.blueprint);
            if (!mesh) continue;
            const auto value=project::evaluate_instance(o.state,source,o.state.viewport.time);
            const auto axes=project::blueprint_translation_axes(o.state,value);
            if (mesh->document().metadata.contains("coordinates/forward")) {
                require(axes.size()==1,"Saved fleet ship lost its forward handle");
                ++oriented;
            } else {
                require(axes.empty(),"Asteroid acquired an unwanted ship handle");
                ++unoriented;
            }
        }
        require(oriented>5 && unoriented>50,"Fleet fixture did not cover both ships and asteroids");
        fleet_revision_ = o.state.document.revision;
        checkpoint(o, "fleet-00-ready");
        return true;
    });
    // Do not wait for worker pixels between selections: the keyframe panel
    // must react in the input frame even when the preview is still catching up.
    for (int i = 0; i < 24; ++i) {
        const auto time = i % 2 ? 1.F : 0.F;
        const auto text = std::to_string(i % 2);
        add("Select fleet keyframe " + text, [this, time, text](const Observation& o) {
            const auto custom = project::keyframe_name(o.state, time);
            const auto label = (o.selected_keyframe == time ? "> " : "") + text +
                               " s | " + (custom.empty() ? "Keyframe" : custom);
            const auto* target = actionable(button(label));
            if (!target) return false;
            click_at(center(intersection(target->bounds, target->clip)));
            input_frame_ = o.frame;
            return true;
        });
        add("Fleet keyframe responds without a reload", [this, time, text, i](const Observation& o) {
            require(o.frame == input_frame_ + 1 && o.selected_keyframe == time &&
                    o.state.viewport.time == time, "Keyframe selection waited for the worker");
            require(o.state.document.revision == fleet_revision_ && !o.dirty,
                    "Selecting a keyframe authored/reloaded the document");
            const auto* title = find({Role::label, "KEYFRAME / " + text + " seconds", {}});
            require(title != nullptr, "Keyframe inspector was not refreshed in the input frame");
            std::vector<u64> ids;
            std::unordered_set<u64> descendants{title->parent};
            for (const auto& item : tree_.widgets) {
                if (item.id != title->parent && !descendants.contains(item.parent)) continue;
                descendants.insert(item.id);
                if (item.role != Role::scrollbar) ids.push_back(item.id);
            }
            require(ids.size() > 600, "Fleet properties missing from keyframe inspector");
            if (keyframe_widget_ids_.empty()) keyframe_widget_ids_ = ids;
            else require(ids == keyframe_widget_ids_, "Keyframe selection rebuilt its widget tree");
            if (i == 0) checkpoint(o, "fleet-01-keyframe-selected");
            return true;
        });
    }
    wait("Fleet preview catches up after rapid keyframe selection", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(o.state.document.revision == fleet_revision_, "Seek changed the document revision");
        checkpoint(o, "fleet-02-selections-complete");
        return true;
    });
    // Capture all fleet properties in one UI action, then undo the insertion
    // before the existing formation/gizmo workflow. This includes real worker
    // transport, inspector construction, and the captured camera pose.
    click(button("Settings"));
    fill(field("Timeline track limit"), "8192");
    click(button("Apply & save"));
    fill(field("Time"), "0.5");
    add("Seek between fleet keys", [this](const Observation&) {
        key(input::Key::enter); return true;
    });
    add("Capture current fleet pose and insert keyframe", [this](const Observation& o) {
        if (!ready(o)) return false;
        const auto* target=actionable(button("Add keyframe"));
        if (!target) return false;
        insertion_pose_=project::evaluate_scene(o.state,.5F);
        insertion_camera_=project::evaluate_camera(o.state,.5F);
        click_at(center(intersection(target->bounds,target->clip))); return true;
    });
    add("Fleet insertion completes in the input frame without changing pose", [this](const Observation& o) {
        require(o.selected_keyframe==.5F && o.state.document.keyframe_names.contains(.5F),
                "Keyframe insertion did not complete locally");
        require(project::evaluate_scene(o.state,.5F)==insertion_pose_,
                "Inserted fleet key changed the visible scene pose");
        require(project::evaluate_camera(o.state,.5F)==insertion_camera_,
                "Inserted fleet key changed the camera pose");
        return true;
    });
    wait("Inserted fleet frame reaches worker", [this](const Observation& o) {
        if (!ready(o)) return false;
        checkpoint(o,"fleet-02b-inserted-key");
        key(input::Key::z,{.control=true}); return true;
    });
    wait("Undo removes inserted fleet key", [](const Observation& o) {
        return ready(o) && !o.state.document.keyframe_names.contains(.5F);
    });
    add("Restore fleet keyframe editing", [this](const Observation& o) {
        const auto name=project::keyframe_name(o.state,1.F);
        const auto* target=actionable(button("1 s | " + (name.empty()?"Keyframe":name)));
        if (!target) return false;
        click_at(center(intersection(target->bounds,target->clip))); return true;
    });
    click(button("Frame world bounds"));
    click(button("Scene")); // Keyframe editing left the sidebar on another tab.
    add("Reveal fleet instance list after framing", [this](const Observation&) {
        const auto* heading=find({Role::label,"SCENE INSTANCES",{}},false);
        require(heading,"No instance list heading");
        // The heading sits in a section of the scrolling Scene page, so the
        // page's scrollbar is a sibling of that section, not of the heading.
        for(const auto* page=node(heading->parent); page; page=node(page->parent))
            for(const auto& bar:tree_.widgets) if(bar.role==Role::scrollbar && bar.parent==page->id) {
                if(bar.maximum && *bar.maximum>0) drag_scrollbar(bar,0); // Already at the top when nothing scrolls.
                return true;
            }
        throw std::runtime_error("No sidebar scrollbar");
    });
    for (u32 id : {3U,18U}) add("Select fleet formation " + std::to_string(id), [this,id](const Observation& o) {
        if (!ready(o)) return false;
        const auto* instance=project::find_instance(o.state,id);
        require(instance,"Fleet ship missing");
        const auto label=(o.state.viewport.selected_object==id?"> #":
            std::ranges::find(o.selected_instances,id)!=o.selected_instances.end()?"+ #":"#")+
            std::to_string(id)+" "+instance->name;
        const auto* target=actionable(button(label)); if(!target)return false;
        const auto point=center(intersection(target->bounds,target->clip));
        pointer(input::EventKind::pointer_down,point); events_.back().modifiers.shift=id==18;
        pointer(input::EventKind::pointer_up,point); events_.back().modifiers.shift=id==18;
        return true;
    });
    click(button("Keyframe values")); // The keyframe inspector shares the sidebar with the scene list.
    wait("Fleet formation has its shared forward handle", [this](const Observation& o) {
        if(!ready(o) || !o.inspector_ready || !o.translation_gizmo || !o.gizmo_visible)return false;
        require(o.selected_instances.size()==16,"Shift selection did not select all fleet ships");
        for(auto id:o.selected_instances)require(id>=3 && id<=18,"Asteroids were accidentally selected");
        require(o.selected_keyframe==1.F,"Selecting a fleet instance exited keyframe editing");
        const auto* selected=project::find_instance(o.state,18);
        const auto blueprints = project::blueprint_catalog(o.state);
        const auto blueprint = std::ranges::find(blueprints, selected->blueprint, &project::Blueprint::id);
        require(blueprint != blueprints.end(), "Selected fleet instance has no blueprint");
        require(find(button(std::string(blueprint->name) + " / " + selected->name))!=nullptr,
                "Keyframe inspector did not reveal the selected fleet instance");
        require(std::ranges::any_of(tree_.widgets,[](const auto& widget) {
            return widget.focused && widget.visible && widget.label=="Key";
        }),"Selected instance keyframe entry was not focused");
        group_before_=project::evaluate_scene(o.state,o.state.viewport.time).instances;
        const auto value=project::evaluate_instance(o.state,*project::find_instance(o.state,18),o.state.viewport.time);
        forward_axis_=project::blueprint_translation_axes(o.state,value).front().direction;
        forward_before_=value.transform.position;
        forward_after_={forward_before_.x+forward_axis_.x*80,forward_before_.y+forward_axis_.y*80,forward_before_.z+forward_axis_.z*80};
        const auto handle=o.translation_gizmo->handle("Forward / back"); require(handle.has_value(),"No fleet forward handle");
        drag_start_=*handle;
        const auto a=project_point(o,forward_before_),b=project_point(o,forward_after_);
        drag_direction_={b.x-a.x,b.y-a.y};
        drag_pixels_=o.preview_pixels->pixels;
        checkpoint(o,"fleet-03-before-forward-move");
        pointer(input::EventKind::pointer_down,drag_start_);return true;
    });
    add("Drag entire fleet forward beyond minus 100", [this](const Observation& o) {
        require(o.dragging,"Fleet forward gizmo failed to capture");
        pointer(input::EventKind::pointer_move,{drag_start_.x+drag_direction_.x,drag_start_.y+drag_direction_.y});
        input_frame_=o.frame;return true;
    });
    add("Every fleet member crosses the old wall immediately and keeps formation", [this](const Observation& o) {
        require(o.frame==input_frame_+1 && o.dragging,"Fleet move was delayed or rejected");
        for(const auto& before:group_before_) {
            const auto p=position(o,before.id);
            const bool fleet=before.id>=3 && before.id<=18;
            for(unsigned axis=0;axis<3;++axis)
                require(std::abs(p[axis]-before.transform.position[axis]-(fleet?forward_axis_[axis]*80:0))<.002F,
                    "Fleet movement lost spacing or moved an unselected asteroid");
            if(fleet)require(p.z < -100,"Fleet still blocked by the old position boundary");
        }
        pointer(input::EventKind::pointer_up,pointer_);return true;
    });
    wait("Worker renders fleet past the old boundary", [this](const Observation& o) {
        if(!ready(o) || o.dragging)return false;
        require(!equivalent(o.preview_pixels->pixels,drag_pixels_),"Fleet move did not reach rendered output");
        checkpoint(o,"fleet-04-beyond-old-boundary");return true;
    });
    click(button("Save As..."));
    fill(field("Scene path, e.g. scene.vscene", "Save scene as"),(directory_/"fleet-moved.vscene").string());
    click(button("Save", "Save scene as"));
    wait("Moved fleet saves with positions and keys beyond the old wall", [this](const Observation& o) {
        if(o.modal || o.dirty)return false;
        const auto restored=take(project::load_scene(directory_/"fleet-moved.vscene"));
        require(restored.document.timeline==o.state.document.timeline,"Fleet keys were lost when saving");
        require(restored.document.instances==o.state.document.instances,"Fleet positions were lost when saving");
        return true;
    });
    click(button("Undo"));
    wait("One undo restores the fleet formation", [this](const Observation& o) {
        if(!ready(o))return false;
        require(project::evaluate_scene(o.state,o.state.viewport.time).instances==group_before_,"Fleet move was not one undo step");
        return true;
    });
    click(button("Redo"));
    wait("Redo restores the fleet beyond the old boundary", [this](const Observation& o) {
        if(!ready(o))return false;
        require(position(o,18).z < -100 && !o.dirty,"Fleet redo lost the saved movement");return true;
    });
    click(button("Settings"));
    fill(field("Timeline track limit", "EDITOR SETTINGS"), "1");
    click(button("Apply & save", "EDITOR SETTINGS"));
    wait("Lowering the track preference preserves the fleet", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(!o.dirty && o.selected_instances.size()==16,"Track preference changed scene or selection");
        fleet_revision_=o.state.document.revision;
        key(input::Key::c,{.control=true}); return true;
    });
    add("Paste with an intentionally exhausted track budget", [this](const Observation& o) {
        require(o.status.find("Copied 16 instances")!=std::string_view::npos,"Fleet was not copied");
        key(input::Key::v,{.control=true}); return true;
    });
    wait("Rejected paste points to Settings and leaves no partial instances", [this](const Observation& o) {
        if (o.status.find("Settings > Timeline track limit")==std::string_view::npos)return false;
        require(o.state.document.instances.size()==example::asteroids::rock_count+example::asteroids::fleet_count+1 && o.state.document.revision==fleet_revision_ && !o.dirty,
            "Track limit failure partially pasted the fleet");
        return true;
    });
    click(button("Settings"));
    fill(field("Timeline track limit", "EDITOR SETTINGS"), "8192");
    fill(field("Instance limit", "EDITOR SETTINGS"), "8192");
    add("Track-limit settings screenshot", [this](const Observation& o) { checkpoint(o,"fleet-05-track-settings"); return true; });
    click(button("Apply & save", "EDITOR SETTINGS"));
    wait("Track preference persists and applies without restarting", [this](const Observation& o) {
        if (!ready(o))return false;
        const auto saved=take(project::load_settings(directory_/"settings.conf"));
        require(saved.timeline_track_limit==8192,"Track limit was not saved");
        require(saved.instance_limit==8192,"Instance limit was not saved");
        require(o.state.document.revision==fleet_revision_ && !o.dirty,"Preference authored a scene revision");
        key(input::Key::v,{.control=true}); return true;
    });
    wait("Same clipboard pastes successfully after raising the track budget", [this](const Observation& o) {
        if (!ready(o) || !o.inspector_ready)return false;
        require(o.state.document.instances.size()==example::asteroids::rock_count+example::asteroids::fleet_count+17 && o.selected_instances.size()==16,"Raised limit did not unblock paste");
        require(o.status.find("Pasted 16 instances")!=std::string_view::npos,"Successful paste was not reported");
        checkpoint(o,"fleet-06-pasted-after-settings"); return true;
    });
    attitude_workflow();
    click(dropdown("Gizmo")); click(option("Yaw/pitch/roll","Gizmo"));
    click(dropdown("Pivot"));click(option("Individual centers","Pivot"));
    add("Add an asteroid to a ship-only selection",[this](const Observation& o) {
        if(!ready(o))return false;
        const auto* asteroid=project::find_instance(o.state,19); require(asteroid,"Missing asteroid fixture");
        const auto* target=actionable(button("#19 "+asteroid->name)); if(!target)return false;
        const auto point=center(intersection(target->bounds,target->clip));
        pointer(input::EventKind::pointer_down,point); events_.back().modifiers.control=true;
        pointer(input::EventKind::pointer_up,point); events_.back().modifiers.control=true;
        return true;
    });
    wait("Mixed selection falls back to shared translation only",[this](const Observation& o) {
        if(!ready(o))return false;
        require(o.selected_instances.size()==17,"Ctrl-click did not extend ship selection");
        require(!o.rotation_gizmo && o.translation_gizmo && !o.translation_gizmo->handle("Forward / back"),
            "Mixed selection retained ship-specific tools");
        return true;
    });
    click(dropdown("Gizmo"));
    add("Gizmo menu is the intersection across the whole selection",[this](const Observation& o) {
        require(find(option("Move","Gizmo")) && find(option("Rotate","Gizmo")) && find(option("Scale","Gizmo")),
            "Mixed selection lost shared transform gizmos");
        require(!find(option("Forward / back","Gizmo")) && !find(option("Yaw/pitch/roll","Gizmo")),
            "Mixed selection menu exposes incompatible blueprint gizmos");
        checkpoint(o,"fleet-07-common-gizmos"); key(input::Key::escape); return true;
    });
}

void Driver::attitude_workflow() {
    click(dropdown("Gizmo"));
    add("Ship selection offers both custom gizmos",[this](const Observation&) {
        require(find(option("Forward / back","Gizmo")) && find(option("Yaw/pitch/roll","Gizmo")),
            "Ship-specific gizmos are missing from the menu"); return true;
    });
    click(option("Forward / back","Gizmo"));
    wait("Forward-only menu mode has a real handle",[this](const Observation& o) {
        if(!ready(o))return false;
        require(o.translation_gizmo && o.translation_gizmo->handle("Forward / back"),"Forward menu mode has no handle");
        forward_before_=position(o,o.state.viewport.selected_object);
        const auto instance=project::evaluate_instance(o.state,*project::find_instance(o.state,o.state.viewport.selected_object),o.state.viewport.time);
        forward_axis_=project::blueprint_translation_axes(o.state,instance).front().direction;
        pointer(input::EventKind::pointer_move,center(o.viewport));key(input::Key::f);
        return true;
    });
    add("F starts the blueprint forward gizmo",[this](const Observation& o) {
        require(o.dragging && o.gizmo_visible,"F did not start a visible forward gizmo");
        require(find(dropdown("Gizmo"))->text.find("Forward / back")!=std::string::npos,"F did not select its menu entry");
        pointer(input::EventKind::pointer_move,{pointer_.x+35,pointer_.y-35});return true;
    });
    add("F moves only along the blueprint axis",[this](const Observation& o) {
        const auto after=position(o,o.state.viewport.selected_object);
        const Vec3 delta{after.x-forward_before_.x,after.y-forward_before_.y,after.z-forward_before_.z};
        const auto along=gfx::camera_detail::dot(delta,forward_axis_);
        require(std::abs(along)>.0001,"F did not move the selected ship");
        for(unsigned c=0;c<3;++c)require(std::abs(delta[c]-along*forward_axis_[c])<.001,"F moved off the blueprint axis");
        checkpoint(o,"keyboard-forward-"+std::to_string(o.frame));key(input::Key::escape);return true;
    });
    wait("Cancel F and select attitude with T",[this](const Observation& o) {
        if(!ready(o))return false;
        require(position(o,o.state.viewport.selected_object)==forward_before_,"F cancellation lost original placement");
        pointer(input::EventKind::pointer_move,center(o.viewport));key(input::Key::t);return true;
    });
    for(u32 axis=0;axis<3;++axis) {
        add("Grab body-relative ring "+std::to_string(axis),[this,axis](const Observation& o) {
            if(!ready(o) || !o.rotation_gizmo || !o.gizmo_visible)return false;
            const auto& ring=o.rotation_gizmo->rings()[axis];
            require(ring.label==std::array<std::string_view,3>{"Yaw","Pitch","Roll"}[axis],"Custom ring label missing");
            bool found{};
            for(std::size_t i=2;i+8<ring.points.size();++i)
                if(ring.projected[i] && ring.projected[i+8] && o.viewport.contains(ring.points[i]) &&
                    o.viewport.contains(ring.points[i+8]) && o.rotation_gizmo->hit_axis(ring.points[i])==axis) {
                    rotation_start_=ring.points[i]; rotation_end_=ring.points[i+8]; found=true; break;
                }
            require(found,"No pickable body-relative ring");
            group_before_=project::evaluate_scene(o.state,o.state.viewport.time).instances;
            rotation_pixels_=o.preview_pixels->pixels;
            checkpoint(o,"attitude-"+std::to_string(axis)+"-before");
            pointer(input::EventKind::pointer_down,rotation_start_); return true;
        });
        add("Move body-relative ring "+std::to_string(axis),[this](const Observation& o) {
            require(o.dragging,"Attitude ring did not capture");
            pointer(input::EventKind::pointer_move,rotation_end_); input_frame_=o.frame; return true;
        });
        add("Every selected body turns immediately in its own frame "+std::to_string(axis),[this,axis](const Observation& o) {
            require(o.frame==input_frame_+1 && o.dragging,"Attitude rotation waited for worker");
            const auto angle=o.rotation_gizmo->turn().degrees; require(std::abs(angle)>.1,"No angular movement");
            for(const auto& before:group_before_) {
                const auto after=project::evaluate_instance(o.state,*project::find_instance(o.state,before.id),o.state.viewport.time);
                if(std::ranges::find(o.selected_instances,before.id)==o.selected_instances.end()) {
                    require(after==before,"Attitude changed an unselected instance"); continue;
                }
                const auto basis=project::blueprint_attitude_axes(o.state,before.blueprint); require(basis.has_value(),"Ship basis missing");
                const auto expected=project::rotation_math::turn(before.transform.rotation,(*basis)[axis],angle);
                for(unsigned component=0;component<3;++component)
                    require(std::abs(after.transform.rotation[component]-expected[component])<.001F,"Group attitude used wrong local axes");
                const auto center=project::instance_centers(o.state,before.id,{})[0];
                const auto offset=project::rotation_math::direction(before.transform.rotation,center.local);
                for(unsigned c=0;c<3;++c)require(std::abs(center.world[c]-(before.transform.position[c]+offset[c]*before.transform.scale))<.001F,
                    "Individual-center rotation displaced a geometric center");
                require(after.transform.scale==before.transform.scale,"Attitude changed scale");
            }
            pointer(input::EventKind::pointer_up,rotation_end_); return true;
        });
        wait("Worker renders committed body-relative rotation "+std::to_string(axis),[this,axis](const Observation& o) {
            if(!ready(o) || o.dragging)return false;
            require(!equivalent(o.preview_pixels->pixels,rotation_pixels_),"Attitude rotation did not affect actual rendering");
            checkpoint(o,"attitude-"+std::to_string(axis)+"-turned"); return true;
        });
        click(button("Undo"));
        wait("One undo restores every selected orientation "+std::to_string(axis),[this](const Observation& o) {
            if(!ready(o))return false;
            require(project::evaluate_scene(o.state,o.state.viewport.time).instances==group_before_,"Attitude undo lost original transforms");
            return true;
        });
    }
    click(dropdown("Gizmo")); click(option("Move","Gizmo"));
}

void Driver::choose_blueprint(std::string label, std::function<project::BlueprintId()> target,
                              std::string screenshot) {
    click(dropdown("View"));
    add("Choose View mesh " + label, [this, target, screenshot = std::move(screenshot)](const Observation& o) {
        assert_view_menu(o);
        const auto* choice = find(option(mesh_view_label(o.state, target()), "View"));
        if (!choice) return false;
        if (!screenshot.empty()) checkpoint(o, screenshot);
        click_at(center(intersection(choice->bounds, choice->clip)));
        return true;
    });
}

void Driver::select_viewport(std::string label, std::function<u32()> target) {
    add("Select " + label + " through viewport", [this, target](const Observation& o) {
        if (!ready(o)) return false;
        const auto object = target();
        const auto origin = project_point(o, position(o, object));
        std::optional<Vec2> visible_surface;
        // An imported ship can overlap the cube at its origin. Find a bounded
        // visible surface point using the public read-only picker, then exercise
        // the real input/selection/overlay path; never assign selected_object.
        for (int radius = 0; radius <= 8 && !visible_surface; ++radius)
            for (int x = -radius; x <= radius && !visible_surface; ++x)
                for (int y = -radius; y <= radius && !visible_surface; ++y) {
                    if (std::max(std::abs(x), std::abs(y)) != radius) continue;
                    const Vec2 point{origin.x + static_cast<f32>(x) * 14, origin.y + static_cast<f32>(y) * 14};
                    if (!o.viewport.contains(point)) continue;
                    const Vec2 normalized{(point.x - o.viewport.x) / o.viewport.width,
                                           (point.y - o.viewport.y) / o.viewport.height};
                    if (project::pick_object(o.state, normalized, o.preview_extent) == object)
                        visible_surface = point;
                }
        require(visible_surface.has_value(), "No visible surface found for viewport selection");
        pointer(input::EventKind::pointer_down, *visible_surface);
        input_frame_ = o.frame;
        selection_dirty_ = o.dirty;
        return true;
    });
    add("Same-frame local selection gizmo " + label, [this, target](const Observation& o) {
        const auto object = target();
        require(o.frame == input_frame_ + 1, "Selection feedback was deferred to another UI frame");
        require(o.state.viewport.selected_object == object, "Viewport selected the wrong object");
        require(o.gizmo_visible, "Selected object's gizmo is absent on the selection frame");
        require(o.dirty == selection_dirty_, "Changing selection dirtied the authored scene");
        const auto expected = project_point(o, position(o, object));
        const auto& view_draw=o.viewport_detached ? *o.viewport_draw_list : o.draw_list;
        if (o.translation_gizmo) require(std::ranges::any_of(view_draw.commands, [&](const auto& command) {
            const auto* box = std::get_if<ui::BoxDraw>(&command);
            return box && box->radius == 4 && box->color == Vec4{1, 1, 1, 1} &&
                   std::hypot(center(box->rect).x - expected.x, center(box->rect).y - expected.y) < 2;
        }), "Same-frame gizmo is not at the selected object's projected origin");
        if (o.rotation_gizmo) require(o.rotation_gizmo->visible(),"Rotation gizmo missing on selection frame");
        if (o.scale_gizmo) require(o.scale_gizmo->visible(),"Scale gizmo missing on selection frame");
        return true;
    });
    for (int frame = 0; frame < 6; ++frame)
        add("Held selection keeps gizmo visible " + label + " / " + std::to_string(frame),
            [this,target](const Observation& o) {
                require(o.state.viewport.selected_object==target(),"Held selection changed target");
                require(o.gizmo_visible,"Gizmo blinked while selection owns LMB");
                require(!o.dragging && !o.box_selecting,"Holding still unexpectedly began a transform or box drag");
                require(o.dirty==selection_dirty_,"Held selection modified the document");
                return true;
            });
    add("Release entity selection " + label,[this](const Observation&) {
        pointer(input::EventKind::pointer_up,pointer_); return true;
    });
    add("Selection release preserves gizmo " + label,[](const Observation& o) {
        require(o.gizmo_visible,"Selection release blinked the gizmo"); return true;
    });
}

void Driver::drag(std::string name, bool cancel, f32 distance) {
    add(name + ": press X gizmo", [this](const Observation& o) {
        if (!ready(o) || !o.inspector_ready || !o.gizmo_visible) return false;
        drag_original_ = position(o, imported_);
        drag_pixels_ = o.preview_pixels->pixels;
        const auto origin = project_point(o, drag_original_);
        auto shifted = drag_original_;
        shifted.x += .01F;
        const auto along = project_point(o, shifted);
        const auto length = std::hypot(along.x - origin.x, along.y - origin.y);
        require(length > .01F, "X gizmo axis is view-parallel");
        drag_direction_ = {(along.x - origin.x) / length, (along.y - origin.y) / length};
        drag_start_ = {origin.x + drag_direction_.x * 30, origin.y + drag_direction_.y * 30};
        pointer(input::EventKind::pointer_move, drag_start_);
        pointer(input::EventKind::pointer_down, drag_start_);
        return true;
    });
    add(name + ": pointer capture", [](const Observation& o) {
        require(o.dragging, "Gizmo press did not capture an actual UI drag");
        return true;
    });
    for (int sample = 1; sample <= 6; ++sample) {
        add(name + ": move " + std::to_string(sample), [this, sample, distance](const Observation& o) {
            require(o.dragging, "Drag capture was lost while awaiting worker acknowledgement");
            const auto delta = distance * static_cast<f32>(sample) / 6;
            pointer(input::EventKind::pointer_move,
                    {drag_start_.x + drag_direction_.x * delta, drag_start_.y + drag_direction_.y * delta});
            input_frame_ = o.frame;
            return true;
        });
        add(name + ": same-frame local move " + std::to_string(sample), [this](const Observation& o) {
            require(o.frame == input_frame_ + 1 && o.dragging,
                    "Pointer motion was not applied during its input frame");
            require(position(o, imported_) != drag_original_, "Live gizmo motion did not update authored position");
            return true;
        });
    }
    wait(name + ": real worker draws while held", [this](const Observation& o) {
        require(o.dragging, "Drag ended before the button was released");
        if (!ready(o)) return false;
        require(!equivalent(o.preview_pixels->pixels, drag_pixels_), "Held drag did not change real preview pixels");
        return true;
    });
    add(name + (cancel ? ": Escape" : ": release"), [this, cancel](const Observation&) {
        if (cancel) key(input::Key::escape);
        pointer(input::EventKind::pointer_up, pointer_);
        return true;
    });
    wait(name + ": final result", [this, cancel](const Observation& o) {
        if (!ready(o) || o.dragging) return false;
        if (cancel) {
            require(position(o, imported_) == drag_original_, "Escape did not restore the drag's original position");
            require(equivalent(o.preview_pixels->pixels, drag_pixels_), "Escape did not restore real preview pixels");
        } else require(position(o, imported_) != drag_original_, "Release lost the committed translation");
        return true;
    });
}

void Driver::forward_workflow() {
    fill(field("Y", "Rotation (degrees)"), "55");
    click(button("Apply instance transform"));
    wait("Rotated ship has a blueprint forward handle", [this](const Observation& o) {
        if (!ready(o) || !o.inspector_ready || !o.translation_gizmo || !o.gizmo_visible ||
            project::instance_transform(o.state,imported_)->rotation.y!=55) return false;
        const auto value=project::evaluate_instance(o.state,*project::find_instance(o.state,imported_),o.state.viewport.time);
        const auto axes=project::blueprint_translation_axes(o.state,value);
        require(axes.size()==1,"Imported spaceship lost its blueprint axis");
        forward_axis_=axes[0].direction;
        forward_before_=value.transform.position;
        auto handle=o.translation_gizmo->handle("Forward / back");
        require(handle && o.viewport.contains(*handle),"Ship forward handle is missing or outside viewport");
        require(std::ranges::any_of(o.draw_list.commands,[](const auto& command) {
            const auto* text=std::get_if<ui::TextDraw>(&command);
            return text && text->text=="Forward / back";
        }),"Blueprint handle has no identifying label");
        drag_start_=*handle;
        const auto a=project_point(o,forward_before_);
        const auto b=project_point(o,{forward_before_.x+forward_axis_.x*.01F,
            forward_before_.y+forward_axis_.y*.01F,forward_before_.z+forward_axis_.z*.01F});
        const auto length=std::hypot(b.x-a.x,b.y-a.y);
        require(length>.001F,"Test forward axis is view parallel");
        drag_direction_={(b.x-a.x)/length,(b.y-a.y)/length};
        drag_pixels_=o.preview_pixels->pixels;
        checkpoint(o,"08d-blueprint-forward-handle");
        pointer(input::EventKind::pointer_down,drag_start_);
        return true;
    });
    add("Forward handle captures and moves",[this](const Observation& o) {
        require(o.dragging,"Blueprint handle did not capture pointer");
        pointer(input::EventKind::pointer_move,
            {drag_start_.x+drag_direction_.x*22,drag_start_.y+drag_direction_.y*22});
        input_frame_=o.frame;
        return true;
    });
    add("Forward translation is immediate and stays on the rotated axis",[this](const Observation& o) {
        require(o.frame==input_frame_+1 && o.dragging,"Blueprint drag lagged or lost capture");
        forward_after_=position(o,imported_);
        const Vec3 d{forward_after_.x-forward_before_.x,forward_after_.y-forward_before_.y,forward_after_.z-forward_before_.z};
        const auto along=d.x*forward_axis_.x+d.y*forward_axis_.y+d.z*forward_axis_.z;
        require(along>0.01F,"Forward arrow did not move toward the ship's nose");
        for(std::size_t i=0;i<3;++i) require(std::abs(d[i]-along*forward_axis_[i])<1e-4F,"Drag followed world axes, not blueprint orientation");
        require(project::instance_mesh(o.state,imported_)->document()==*imported_document_,"Forward drag modified shared geometry");
        pointer(input::EventKind::pointer_up,pointer_);
        return true;
    });
    wait("Worker renders committed forward drag",[this](const Observation& o) {
        if(!ready(o) || o.dragging) return false;
        require(!equivalent(o.preview_pixels->pixels,drag_pixels_),"Forward drag did not reach real worker rendering");
        require(position(o,imported_)==forward_after_,"Forward position was lost on release");
        checkpoint(o,"08e-blueprint-forward-moved");
        key(input::Key::z,{.control=true});
        return true;
    });
    wait("Undo forward drag restores position",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(position(o,imported_)==forward_before_,"Forward Undo lost original position");
        key(input::Key::y,{.control=true});
        return true;
    });
    wait("Redo forward drag restores placement",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(position(o,imported_)==forward_after_,"Forward Redo lost placement");
        key(input::Key::z,{.control=true});
        return true;
    });
    wait("Restore position before continuing timeline walkthrough",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(position(o,imported_)==forward_before_,"Second forward Undo failed");
        key(input::Key::z,{.control=true});
        return true;
    });
    wait("Restore orientation before continuing timeline walkthrough",[this](const Observation& o) {
        return ready(o) && o.inspector_ready && project::instance_transform(o.state,imported_)->rotation.y==0;
    });
}
void Driver::sidebar_resize_workflow() {
    add("Sidebar lists initially start at their very top", [this](const Observation& o) {
        const auto* heading = find({Role::label,"SCENE INSTANCES",{}}); require(heading,"Missing scene panel");
        std::size_t checked{};
        for (const auto& widget : tree_.widgets)
            if (widget.role==Role::scrollbar && inside(widget,node(heading->parent)->parent)) {
                require(widget.number && *widget.number==0.F,"A sidebar list was auto-scrolled on startup");
                ++checked;
            }
        require(checked>=3,"Missing independently scrolling sidebar and lists");
        require(!o.dirty,"Initial scrolling edited the document"); return true;
    });
    for (const auto label : {"Resize scene and keyframe panels", "Resize scene and region lists", "Resize region and blueprint lists",
                            "Resize blueprint list and tools", "Resize tools and region creation"}) {
        struct DragState { f32 value{}; Vec2 start{}; u64 revision{}, sequence{}; ui::Rect viewport{}; f32 delta{}; };
        auto drag = std::make_shared<DragState>();
        const Locator splitter{Role::splitter,label,{}};
        add(std::string("Grab divider: ")+label, [this,splitter,drag](const Observation& o) {
            const auto* handle = actionable(splitter); if (!handle) return false;
            require(handle->number.has_value(),"Divider has no logical-pixel value");
            *drag = {*handle->number, center(handle->bounds), o.state.document.revision, o.state.viewport.sequence,o.viewport};
            drag->delta=-std::min(24.F,(*handle->number-*handle->minimum)*.45F);
            if(std::abs(drag->delta)<1)drag->delta=std::min(24.F,(*handle->maximum-*handle->number)*.45F);
            pointer(input::EventKind::pointer_down,drag->start); return true;
        });
        add("Grabbing a divider captures input without jumping", [this,splitter,drag](const Observation&) {
            const auto* handle = find(splitter); require(handle && handle->pressed,"Divider did not capture input");
            require(std::abs(*handle->number-drag->value)<.01F,"Divider jumped on initial press");
            pointer(input::EventKind::pointer_move,{drag->start.x,drag->start.y+drag->delta}); return true;
        });
        add("Divider and panels follow the same input frame without preview work", [this,splitter,drag](const Observation& o) {
            const auto* handle=find(splitter); require(handle && handle->pressed,"Divider lost capture during layout");
            require(std::abs(*handle->number-(drag->value+drag->delta))<.05F,"Divider value lags input");
            require(std::abs(center(handle->bounds).y-(drag->start.y+drag->delta))<.05F,"Divider geometry lags input");
            require(o.state.document.revision==drag->revision && o.state.viewport.sequence==drag->sequence && !o.dirty,
                    "Resizing authored scene or preview changes");
            require(o.viewport.width==drag->viewport.width && o.viewport.height==drag->viewport.height,
                    "Sidebar division changed viewport render resolution");
            pointer(input::EventKind::pointer_move,{drag->start.x,drag->start.y+2*drag->delta}); return true;
        });
        add("Repeated divider movement does not compound after relayout", [this,splitter,drag](const Observation&) {
            const auto* handle=find(splitter); require(handle && handle->pressed,"Divider released early");
            require(std::abs(*handle->number-(drag->value+2*drag->delta))<.05F,"Divider movement compounded");
            key(input::Key::escape); return true;
        });
        add("Escape restores the original pane sizes", [this,splitter,drag](const Observation& o) {
            const auto* handle=find(splitter); require(handle && !handle->pressed,"Escape did not release divider");
            require(std::abs(*handle->number-drag->value)<.05F &&
                    std::abs(center(handle->bounds).y-drag->start.y)<.05F,"Escape lost original sizes");
            require(o.state.document.revision==drag->revision && o.state.viewport.sequence==drag->sequence,"Cancel changed scene state");
            pointer(input::EventKind::pointer_up,drag->start);
            return true;
        });
        add("Commit a divider resize", [this,splitter,drag](const Observation&) {
            const auto* handle=find(splitter); require(handle,"Missing divider");
            pointer(input::EventKind::pointer_down,center(handle->bounds));
            pointer(input::EventKind::pointer_up,{drag->start.x,drag->start.y+drag->delta}); return true;
        });
        click(button("Instance properties")); click(button("Scene"));
        add("Sidebar tab changes preserve resized proportions", [this,splitter,drag](const Observation& o) {
            const auto* handle=find(splitter); require(handle,"Divider not restored with Scene tab");
            require(std::abs(*handle->number-(drag->value+drag->delta))<.05F,"Tab switch reset the divider");
            require(o.state.document.revision==drag->revision && !o.dirty,"Divider resize entered document history");
            checkpoint(o,"01-divider-"+splitter.label);
            pointer(input::EventKind::pointer_down,center(handle->bounds));
            pointer(input::EventKind::pointer_up,drag->start); return true;
        });
        add("Restoring divider preserves document and selection", [this,splitter,drag](const Observation& o) {
            const auto* handle=find(splitter); require(handle && std::abs(*handle->number-drag->value)<.05F,"Divider did not restore");
            require(!o.selected_keyframe && !o.dirty && o.state.document.revision==drag->revision,"Resizing changed editing state");
            return true;
        });
    }
}

void Driver::scroll_workflow() {
    // Force a short sidebar to exercise outer overflow independently of each
    // list/tool section's own scroll area.
    auto original_height=std::make_shared<f32>();
    add("Shorten Scene panel for nested scrolling",[this,original_height](const Observation&) {
        const auto* divider=actionable({Role::splitter,"Resize scene and keyframe panels",{}});
        if(!divider)return false;
        *original_height=*divider->number;
        const auto start=center(divider->bounds);
        const auto height=std::clamp(450.F,*divider->minimum,*divider->maximum);
        pointer(input::EventKind::pointer_down,start);
        pointer(input::EventKind::pointer_up,{start.x,start.y+height-*divider->number});
        return true;
    });
    click({Role::checkbox,"World bounds",{}});
    wait("World bounds exposes numerical controls in the Scene page",[this](const Observation&) {
        return find(button("Apply world bounds"),false)!=nullptr;
    });
    add("Drag instance-list scrollbar without changing scene selection", [this](const Observation& o) {
        const auto* heading = find({Role::label, "SCENE INSTANCES", {}});
        require(heading, "Missing sidebar heading");
        for (const auto& bar : tree_.widgets) {
            if (bar.role != Role::scrollbar) continue;
            if (bar.parent == node(heading->parent)->parent) sidebar_bar_ = bar.id;
            const auto* owner = node(bar.parent);
            if (owner && owner->parent == heading->parent && (!instance_bar_ || bar.parent < node(instance_bar_)->parent))
                instance_bar_ = bar.id;
        }
        const auto* outer = node(sidebar_bar_);
        const auto* inner = node(instance_bar_);
        require(outer && inner && outer->visible && inner->visible, "Nested visible scrollbars missing");
        require(outer->thumb && *outer->maximum > 0 && inner->maximum,
                "Sidebar must overflow; the taller instance list may fit this short scene");
        sidebar_before_ = *outer->number;
        scroll_revision_ = o.state.document.revision;
        if (*inner->maximum > 0) drag_scrollbar(*inner, 0);
        return true;
    });
    add("Instance thumb changes only its own scroll offset", [this](const Observation& o) {
        require(std::abs(*node(instance_bar_)->number) < .1F, "Instance scrollbar did not reach top");
        require(*node(sidebar_bar_)->number == sidebar_before_, "Inner drag moved outer panel");
        require(o.state.document.revision == scroll_revision_ && o.state.viewport.selected_object == 2,
                "Scrollbar click selected/deleted an object or authored a revision");
        list_before_ = *node(instance_bar_)->number;
        drag_scrollbar(*node(sidebar_bar_), *node(sidebar_bar_)->maximum);
        return true;
    });
    add("Whole-sidebar scrollbar reveals region creation without moving toolbar controls", [this](const Observation& o) {
        const auto* bar = node(sidebar_bar_);
        require(std::abs(*bar->number - *bar->maximum) < .1F, "Outer scrollbar did not reach bottom");
        require(*node(instance_bar_)->number == list_before_, "Outer scrollbar changed list offset");
        const auto* creation = find(button("Add region"));
        require(creation && intersection(creation->bounds, creation->clip).height >= creation->bounds.height - 1,
                "Scrolling did not reveal region creation button");
        const auto* camera = find(button("Camera settings"));
        require(camera && camera->bounds.y < o.viewport.y, "Sidebar scrolling moved camera settings off its toolbar");
        require(o.state.document.revision == scroll_revision_, "Scrolling changed the scene revision");
        checkpoint(o, "01a-sidebar-scrolled");
        return true;
    });
    click(button("Instance properties"));
    click(button("Scene"));
    add("Tab roundtrip preserves independent sidebar and instance-list scroll offsets",[this](const Observation& o) {
        const auto* bar=node(sidebar_bar_);
        require(bar && std::abs(*bar->number-*bar->maximum)<.1F,"Switching tabs reset sidebar scrolling");
        require(*node(instance_bar_)->number==list_before_,"Switching tabs reset instance-list scrolling");
        require(o.state.document.revision==scroll_revision_,"Switching tabs authored a revision");
        drag_scrollbar(*bar,0);
        return true;
    });
    add("Return sidebar to top and move list to its bottom", [this](const Observation&) {
        require(std::abs(*node(sidebar_bar_)->number) < .1F, "Sidebar did not return to top");
        if (*node(instance_bar_)->maximum > 0)
            drag_scrollbar(*node(instance_bar_), *node(instance_bar_)->maximum);
        return true;
    });
    add("Wheel at the list edge bubbles into the sidebar", [this](const Observation&) {
        const auto* bar = node(instance_bar_);
        require(std::abs(*bar->number - *bar->maximum) < .1F, "List did not reach bottom");
        pointer_ = {bar->bounds.x - 12, bar->bounds.y + bar->bounds.height * .5F};
        events_.push_back({.kind = input::EventKind::scroll, .position = pointer_, .scroll = {0, -1}});
        return true;
    });
    add("Nested wheel changed outer offset without changing state", [this](const Observation& o) {
        require(*node(sidebar_bar_)->number > 0, "Wheel was swallowed at the nested list edge: inner=" +
            std::to_string(*node(instance_bar_)->number) + "/" + std::to_string(*node(instance_bar_)->maximum) +
            " outer=" + std::to_string(*node(sidebar_bar_)->number) + "/" + std::to_string(*node(sidebar_bar_)->maximum));
        require(o.state.document.revision == scroll_revision_ && o.state.viewport.selected_object == 2,
                "Nested scrolling altered scene state");
        drag_scrollbar(*node(sidebar_bar_), 0);
        return true;
    });
    click({Role::checkbox,"World bounds",{}});
    add("Restore Scene panel after nested scrolling",[this,original_height](const Observation&) {
        const auto* divider=actionable({Role::splitter,"Resize scene and keyframe panels",{}});
        if(!divider)return false;
        const auto start=center(divider->bounds);
        pointer(input::EventKind::pointer_down,start);
        pointer(input::EventKind::pointer_up,{start.x,start.y+*original_height-*divider->number});
        return true;
    });
}

void Driver::rotation_workflow() {
    click(dropdown("Gizmo"));
    click(option("Move", "Gizmo"));
    add("Ctrl Left wraps from Move to the ship attitude gizmo", [this](const Observation& o) {
        if(!ready(o))return false;
        key(input::Key::left,{.control=true});return true;
    });
    wait("Keyboard selects the blueprint-specific attitude gizmo", [this](const Observation& o) {
        if(!ready(o))return false;
        require(o.rotation_gizmo && find(dropdown("Gizmo"))->text.find("Yaw/pitch/roll")!=std::string::npos,
                "Ctrl Left did not cycle to the ship custom gizmo");
        key(input::Key::right,{.control=true});return true;
    });
    wait("Ctrl Right wraps back to Move", [this](const Observation& o) {
        if(!ready(o))return false;
        require(o.translation_gizmo && !o.rotation_gizmo,"Ctrl Right did not wrap to Move");
        key(input::Key::right,{.control=true});return true;
    });
    add("Grab the spaceship Z rotation ring", [this](const Observation& o) {
        if (!ready(o) || !o.rotation_gizmo || !o.gizmo_visible) return false;
        const auto& ring = o.rotation_gizmo->rings()[2];
        bool found{};
        for (std::size_t i = 0; i + 9 < ring.points.size(); ++i) {
            if (!ring.projected[i] || !ring.projected[i + 9] ||
                !o.viewport.contains(ring.points[i]) || !o.viewport.contains(ring.points[i + 9]) ||
                o.rotation_gizmo->hit_axis(ring.points[i]) != 2) continue;
            rotation_start_ = ring.points[i]; rotation_end_ = ring.points[i + 9]; found = true; break;
        }
        require(found, "No selectable visible Z rotation arc");
        rotation_before_ = project::instance_transform(o.state, imported_)->rotation;
        rotation_pixels_ = o.preview_pixels->pixels;
        pointer(input::EventKind::pointer_down, rotation_start_);
        return true;
    });
    add("Rotation captures its first input frame", [this](const Observation& o) {
        require(o.dragging, "Rotation ring did not capture pointer");
        pointer(input::EventKind::pointer_move, rotation_end_);
        input_frame_ = o.frame;
        return true;
    });
    add("Rotation updates locally on the same input frame", [this](const Observation& o) {
        require(o.frame == input_frame_ + 1 && o.dragging, "Rotation feedback waited for worker ACK");
        rotation_after_ = project::instance_transform(o.state, imported_)->rotation;
        require(rotation_after_.z != rotation_before_.z && rotation_after_.x == rotation_before_.x &&
                    rotation_after_.y == rotation_before_.y, "Z ring did not edit the correct instance component");
        require(project::instance_mesh(o.state, imported_)->document() == *imported_document_,
                "Rotation edited blueprint geometry");
        return true;
    });
    wait("Live rotation reaches rendered ship before release", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(o.dragging && !equivalent(o.preview_pixels->pixels, rotation_pixels_),
                "Rendered model did not rotate during drag");
        checkpoint(o, "05a-live-rotation");
        pointer(input::EventKind::pointer_up, rotation_end_);
        return true;
    });
    wait("Rotation release keeps instance transform", [this](const Observation& o) {
        if (!ready(o) || o.dragging) return false;
        require(project::instance_transform(o.state, imported_)->rotation == rotation_after_,
                "Release lost rotated orientation");
        return true;
    });
    click(button("Undo"));
    wait("Undo restores entire rotation gesture", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(project::instance_transform(o.state, imported_)->rotation == rotation_before_, "Undo lost rotation baseline");
        require(equivalent(o.preview_pixels->pixels, rotation_pixels_), "Undo failed to restore original rendered ship");
        return true;
    });
    click(button("Redo"));
    wait("Redo restores rotation", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(project::instance_transform(o.state, imported_)->rotation == rotation_after_, "Redo lost rotation result");
        return true;
    });
    add("Begin a rotation to cancel", [this](const Observation& o) {
        if (!ready(o) || !o.rotation_gizmo) return false;
        const auto& ring = o.rotation_gizmo->rings()[2];
        for (std::size_t i = 0; i + 6 < ring.points.size(); ++i) {
            if (!ring.projected[i] || !ring.projected[i + 6] || !o.viewport.contains(ring.points[i]) ||
                !o.viewport.contains(ring.points[i + 6]) || o.rotation_gizmo->hit_axis(ring.points[i]) != 2) continue;
            rotation_start_ = ring.points[i]; rotation_end_ = ring.points[i + 6];
            pointer(input::EventKind::pointer_down, rotation_start_);
            pointer(input::EventKind::pointer_move, rotation_end_);
            return true;
        }
        throw std::runtime_error("Missing cancel-test rotation arc");
    });
    add("Escape cancels a changed rotation", [this](const Observation& o) {
        require(o.dragging && project::instance_transform(o.state, imported_)->rotation != rotation_after_,
                "Cancel test did not change orientation");
        key(input::Key::escape);
        pointer(input::EventKind::pointer_up, rotation_end_);
        return true;
    });
    wait("Cancellation restores exact orientation", [this](const Observation& o) {
        if (!ready(o) || o.dragging) return false;
        require(project::instance_transform(o.state, imported_)->rotation == rotation_after_,
                "Escape failed to restore orientation");
        return true;
    });
    click(button("Undo"));
    wait("Cancel did not add an undo entry", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(project::instance_transform(o.state, imported_)->rotation == rotation_before_,
                "Cancelled gesture polluted history");
        require(equivalent(o.preview_pixels->pixels, rotation_pixels_), "Original rotation pixels not restored");
        return true;
    });
    click(dropdown("Pivot"));click(option("Custom point","Pivot"));
    click({Role::checkbox,"Move rotation origin",{}});
    auto origin_revision=std::make_shared<u64>();
    auto origin_before=std::make_shared<Vec3>();
    add("Move the custom rotation origin with an ordinary XYZ handle",[this,origin_revision,origin_before](const Observation& o) {
        if(!ready(o))return false;
        require(o.rotation_origin_gizmo,"No origin gizmo");
        auto point=o.rotation_origin_gizmo->handle("X");require(point.has_value(),"No origin X handle");
        *origin_revision=o.state.document.revision;*origin_before=o.rotation_origin;
        pointer(input::EventKind::pointer_down,*point);
        pointer(input::EventKind::pointer_move,{point->x+40,point->y});
        pointer(input::EventKind::pointer_up,{point->x+40,point->y});return true;
    });
    add("Origin movement is private and leaves authored objects unchanged",[this,origin_revision,origin_before](const Observation& o) {
        require(o.rotation_origin!=*origin_before,"Origin did not move");
        require(o.state.document.revision==*origin_revision,"Moving the rotation origin dirtied the document");
        checkpoint(o,"05b-custom-rotation-origin");return true;
    });
    click({Role::checkbox,"Move rotation origin",{}});
    click(dropdown("Pivot"));click(option("Selection center","Pivot"));
    click(dropdown("Gizmo"));click(option("Free rotate","Gizmo"));
    auto free_before=std::make_shared<project::InstanceTransform>();
    auto free_camera=std::make_shared<project::CameraPose>();
    auto free_center=std::make_shared<Vec3>();
    add("MMB starts free rotation without moving the camera",[this,free_before,free_camera,free_center](const Observation& o) {
        if(!ready(o)||!o.rotation_gizmo||!o.gizmo_visible)return false;
        *free_before=*project::instance_transform(o.state,imported_);
        *free_camera=project::view_camera(o.state);
        *free_center=project::selection_center(project::instance_centers(o.state,imported_,std::array{imported_}));
        rotation_start_={o.viewport.x+o.viewport.width*.7F,o.viewport.y+o.viewport.height*.3F};
        rotation_end_={rotation_start_.x+90,rotation_start_.y+55};
        pointer(input::EventKind::pointer_down,rotation_start_,2);return true;
    });
    add("Free rotation captures MMB",[this](const Observation& o) {
        require(o.dragging,"Free rotation did not capture MMB");
        pointer(input::EventKind::pointer_move,rotation_end_);input_frame_=o.frame;return true;
    });
    add("MMB changes orientation immediately and preserves the center",[this,free_before,free_camera,free_center](const Observation& o) {
        require(o.frame==input_frame_+1 && o.dragging,"MMB rotation waited for the worker");
        require(project::view_camera(o.state)==*free_camera,"MMB also moved the camera");
        require(project::instance_transform(o.state,imported_)->rotation!=free_before->rotation,"MMB did not rotate the ship");
        const auto center=project::selection_center(project::instance_centers(o.state,imported_,std::array{imported_}));
        for(unsigned c=0;c<3;++c)require(std::abs(center[c]-(*free_center)[c])<.0001F,"MMB moved the rotation center");
        pointer(input::EventKind::pointer_up,rotation_end_,2);return true;
    });
    wait("Released MMB publishes the rotated ship",[this](const Observation& o) {
        if(!ready(o)||o.dragging)return false;
        checkpoint(o,"05c-free-rotation");return true;
    });
    click(button("Undo"));
    wait("One undo restores the MMB transform",[this,free_before,free_camera](const Observation& o) {
        if(!ready(o))return false;
        require(*project::instance_transform(o.state,imported_)==*free_before,"MMB undo did not restore the entire transform");
        require(project::view_camera(o.state)==*free_camera,"MMB changed the viewing camera");return true;
    });
    click(dropdown("Gizmo"));
    click(option("Move", "Gizmo"));
}

void Driver::convenience_workflow() {
    click(dropdown("Gizmo"));
    click(option("Scale","Gizmo"));
    wait("Remember active instance before gizmo-only preview",[this](const Observation& o) {
        if(!ready(o))return false;
        normal_pixels_=o.preview_pixels->pixels;
        scroll_revision_=o.state.document.revision;
        return true;
    });
    click({Role::checkbox,"Selected: gizmo only",{}});
    wait("Gizmo-only preview hides surface but retains selection and controls",[this](const Observation& o) {
        if(!ready(o))return false;
        if(!o.scale_gizmo || !o.scale_gizmo->visible())return false;
        require(o.state.viewport.gizmo_only,"Gizmo-only toggle was not applied");
        require(o.state.viewport.selected_object==imported_,"Gizmo-only changed selection");
        require(o.state.document.revision==scroll_revision_,"Gizmo-only changed document revision");
        require(!equivalent(o.preview_pixels->pixels,normal_pixels_),"Gizmo-only did not change the rendered surface");
        return true;
    });
    click({Role::checkbox,"Selected: gizmo only",{}});
    wait("Disabling gizmo-only restores the active surface",[this](const Observation& o) {
        if(!ready(o))return false;
        require(!o.state.viewport.gizmo_only,"Gizmo-only toggle did not clear");
        require(o.state.document.revision==scroll_revision_,"Restoring surface changed document revision");
        require(equivalent(o.preview_pixels->pixels,normal_pixels_),"Gizmo-only changed authored appearance");
        return true;
    });
    fill(field("Maximum instance scale"),"30");
    click(button("Apply scale limits"));
    add("Grab uniform scale handle",[this](const Observation& o) {
        if(!ready(o) || !o.scale_gizmo || !o.scale_gizmo->visible()) return false;
        scale_pointer_=o.scale_gizmo->handle();
        pointer(input::EventKind::pointer_down,scale_pointer_);
        return true;
    });
    add("Drag scale while holding the mouse",[this](const Observation& o) {
        require(o.dragging,"Scale handle did not capture");
        // Cross the inspector's old maximum while the local gizmo updates
        // immediately and its replacement worker schema is still in flight.
        scale_pointer_.x+=300; scale_pointer_.y-=100;
        pointer(input::EventKind::pointer_move,scale_pointer_); input_frame_=o.frame;
        return true;
    });
    add("Scale changes locally on the input frame",[this](const Observation& o) {
        require(o.frame==input_frame_+1 && o.dragging,"Scale input was delayed");
        gizmo_scale_=project::instance_transform(o.state,imported_)->scale;
        require(gizmo_scale_>3.F && gizmo_scale_<=30.F,"Scale handle did not cross the old inspector range");
        require(project::instance_mesh(o.state,imported_)->document()==*imported_document_,"Scale changed blueprint");
        return true;
    });
    wait("Scaled pixels arrive before release",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.dragging && !equivalent(o.preview_pixels->pixels,scaled_pixels_),"Scale preview waited for release");
        checkpoint(o,"08a-live-scale-gizmo");
        pointer(input::EventKind::pointer_up,scale_pointer_);
        return true;
    });
    wait("Ctrl+Z undo scale gesture",[this](const Observation& o) {
        if(!ready(o) || o.dragging) return false;
        key(input::Key::z,{.control=true}); return true;
    });
    wait("Ctrl+Z restores complete scale",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(project::instance_transform(o.state,imported_)->scale==.55F,"Ctrl+Z failed to restore scale");
        key(input::Key::y,{.control=true}); return true;
    });
    wait("Ctrl+Y redoes scale",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(project::instance_transform(o.state,imported_)->scale==gizmo_scale_,"Ctrl+Y failed to redo scale");
        key(input::Key::z,{.control=true}); return true;
    });
    wait("Copy the restored instance",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(project::instance_transform(o.state,imported_)->scale==.55F,"Restore before copy failed");
        key(input::Key::c,{.control=true}); return true;
    });
    add("Paste instance",[this](const Observation& o) {
        require(o.status.starts_with("Copied 1 instance"), "Single-instance copy did not report success");
        key(input::Key::v,{.control=true}); input_frame_=o.frame; return true;
    });
    add("Paste selects Move and shows its gizmo in the input frame",[this](const Observation& o) {
        require(o.frame==input_frame_+1,"Paste feedback was delayed");
        require(o.state.document.instances.size()==4 && o.state.viewport.selected_object!=imported_,
                "Paste did not immediately select the new instance");
        require(o.status.starts_with("Pasted 1 instance in place"), "Single-instance paste did not report success");
        require(o.gizmo_visible && !o.scale_gizmo && !o.rotation_gizmo,
                "Pasted instance did not immediately show its move gizmo");
        checkpoint(o,"08b-paste-move-gizmo");
        return true;
    });
    wait("Paste shares blueprint but creates independent instance",[this](const Observation& o) {
        if(!ready(o) || o.state.document.instances.size()!=4) return false;
        pasted_instance_=o.state.viewport.selected_object;
        require(pasted_instance_!=imported_ && project::find_instance(o.state,pasted_instance_)->blueprint==imported_blueprint_,
                "Paste duplicated blueprint or failed to create an instance");
        require(o.state.document.mesh_assets.size()==1,"Paste copied mesh geometry");
        key(input::Key::z,{.control=true}); return true;
    });
    wait("Undo paste restores selection and blueprint",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.state.document.instances.size()==3 && !project::find_instance(o.state,pasted_instance_) &&
                o.state.viewport.selected_object==imported_,"Undo paste failed");
        key(input::Key::z,{.shift=true,.control=true}); return true;
    });
    wait("Ctrl+Shift+Z also redoes paste",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(project::find_instance(o.state,pasted_instance_)!=nullptr,"Alternate redo failed");
        key(input::Key::z,{.control=true}); return true;
    });
    wait("Restore original scene after shortcut tests",[](const Observation& o) {
        return ready(o) && o.state.document.instances.size()==3;
    });
    click(dropdown("Gizmo")); click(option("Move","Gizmo"));
    click(button("Instance properties"));
    add("Press scale slider for live numeric preview",[this](const Observation& o) {
        if(!ready(o) || !o.inspector_ready) return false;
        const auto* slider=actionable({Role::slider,"Scale","Instance transform"});
        if(!slider) return false;
        scale_pointer_={slider->bounds.x+slider->bounds.width*.4F,slider->bounds.y+slider->bounds.height*.5F};
        pointer(input::EventKind::pointer_down,scale_pointer_); input_frame_=o.frame;
        return true;
    });
    add("Scale slider updates on input frame without Apply",[this](const Observation& o) {
        require(o.frame==input_frame_+1 && o.dragging,"Numeric scale preview was delayed");
        require(project::instance_transform(o.state,imported_)->scale!=.55F,"Scale slider still requires Apply");
        return true;
    });
    wait("Live numeric preview renders before release",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.dragging && !equivalent(o.preview_pixels->pixels,scaled_pixels_),"Numeric scale waits for Apply");
        key(input::Key::escape); pointer(input::EventKind::pointer_up,scale_pointer_);
        return true;
    });
    wait("Escape restores numeric scale without an undo entry",[this](const Observation& o) {
        if(!ready(o) || o.dragging) return false;
        require(project::instance_transform(o.state,imported_)->scale==.55F,"Escape did not restore slider scale");
        return true;
    });
}

void Driver::workflow() {
    wait("Initial real worker image", [this](const Observation& o) {
        if (!ready(o) || !o.inspector_ready) return false;
        visible_pixels(o);
        require(!o.dirty, "Fresh scene starts dirty");
        require(!o.selected_keyframe && o.selected_keyframes.empty(),
                "Opening a scene selected a keyframe without user input");
        require(!o.gizmo_visible, "Opening at time zero exposed a scene transform gizmo");
        require(o.extent.width == 1600 && o.extent.height == 1000, "Automation window size is not deterministic");
        require(o.viewport.width > 400 && o.viewport.height > 250, "Preview layout has collapsed");
        for (const auto& locator : {button("Save As..."), button("Import..."), button("Settings")}) {
            const auto* widget = find(locator);
            require(widget != nullptr, "Essential editor action is missing or disabled");
            const auto shown = intersection(widget->bounds, widget->clip);
            require(shown.width >= widget->bounds.width - 1 && shown.height >= widget->bounds.height - 1,
                    "Essential action is clipped by the actual UI layout");
        }
        const auto first_y=find(button("Open scene"))->bounds.y;
        for(const auto label:{"Open scene","Save","Save As...","Import...","Undo","Redo","Logs","Settings"}) {
            const auto item=std::ranges::find_if(tree_.widgets,[&](const auto& widget) {
                return widget.role==Role::button && widget.label==label;
            });
            require(item!=tree_.widgets.end() && item->visible && item->bounds.y==first_y,
                    "Application action is not in the first toolbar: "+std::string(label));
        }
        const auto second_y=find(button("Camera settings"))->bounds.y;
        require(second_y>first_y,"Camera settings is not in the second row");
        for (const auto label : {"Apply to keyframe", "Apply to keyframes"}) {
            const auto item = std::ranges::find_if(tree_.widgets, [&](const auto& widget) {
                return widget.role == Role::button && widget.text == label;
            });
            require(item != tree_.widgets.end() && item->visible && !item->enabled && item->bounds.y == second_y,
                    "Keyframe toolbar action must stay visible but disabled before selection");
            require(item->bounds.x > o.draw_list.logical_size.x - 510, "Apply actions are not pinned to the right");
        }
        for(const auto& locator:std::initializer_list<Locator>{button("Camera settings"),button("Frame world bounds"),
                {Role::checkbox,"FPS",{}},{Role::checkbox,"World bounds",{}},
                {Role::checkbox,"Regions / TODO volumes",{}},dropdown("Region shape")}) {
            const auto* item=find(locator);
            require(item && item->bounds.y==second_y,"Scene control is not in the second toolbar: "+locator.label);
        }
        checkpoint(o, "01-baseline");
        require(!fps_text(o.draw_list),"Preview FPS should initially be hidden");
        return true;
    });
    sidebar_resize_workflow();
    for (const auto shortcut : {input::Key::g, input::Key::r, input::Key::s}) {
        add("Attempt scene transform without selecting a keyframe", [this, shortcut](const Observation& o) {
            pointer(input::EventKind::pointer_move, center(o.viewport));
            key(shortcut);
            return true;
        });
        add("Unselected initial pose stays read-only", [](const Observation& o) {
            require(!o.dragging && !o.gizmo_visible && !o.dirty && !o.selected_keyframe,
                    "A transform shortcut unlocked the initial pose");
            return true;
        });
    }
    const auto fps_revision = std::make_shared<std::pair<u64,u64>>();
    add("Remember scene identity before showing FPS",[fps_revision](const Observation& o) {
        *fps_revision = {o.state.document.revision, o.state.viewport.sequence};
        return true;
    });
    click({Role::checkbox,"FPS",{}});
    wait("FPS overlay measures the preview without editing the scene",[this,fps_revision](const Observation& o) {
        const auto* text = fps_text(o.draw_list);
        if (!text || !text->text.ends_with(" ms (idle)")) return false;
        bool ui_rate{};
        for (const auto& command : o.draw_list.commands)
            if (const auto* draw = std::get_if<ui::TextDraw>(&command))
                ui_rate |= draw->text.starts_with("UI: ") && draw->text.ends_with(" FPS");
        if (!ui_rate) return false;
        require(o.viewport.contains(text->position),"FPS overlay is outside the viewport");
        require(o.state.document.revision==fps_revision->first &&
                o.state.viewport.sequence==fps_revision->second && !o.dirty,
                "FPS toggle changed authored content or sent a viewport update");
        checkpoint(o,"01d-preview-fps");
        return true;
    });
    click({Role::checkbox,"FPS",{}});
    add("FPS toggle removes the overlay immediately",[](const Observation& o) {
        require(!fps_text(o.draw_list),"FPS overlay remains visible after disabling");
        return true;
    });
    struct TabEvidence { u64 revision{}, sequence{}, scene_heading{}; u32 selected{}; };
    auto tabs=std::make_shared<TabEvidence>();
    add("Scene tab uses the right sidebar and leaves the left edge to the viewport",[this,tabs](const Observation& o) {
        const auto* scene=find(button("Scene"));
        const auto* heading=find({Role::label,"SCENE INSTANCES",{}});
        require(scene && scene->selected && heading,"Scene tab is not initially active");
        require(o.viewport.x<20 && o.viewport.width>o.draw_list.logical_size.x*.65F,
                "Old left sidebar still reserves viewport space");
        require(heading->bounds.x>o.viewport.x+o.viewport.width,"Scene page is not in the right sidebar");
        *tabs={o.state.document.revision,o.state.viewport.sequence,heading->id,o.state.viewport.selected_object};
        return true;
    });
    click(button("Instance properties"));
    add("Properties replaces the scene page without changing selection",[this,tabs](const Observation& o) {
        require(!find({Role::label,"SCENE INSTANCES",{}}),"Scene page overlaps properties");
        require(find(button("Instance properties"))->selected,"Properties tab is not emphasized");
        require(find(button("Camera settings")) && find(button("Import...")) &&
                find({Role::checkbox,"World bounds",{}}) && find(dropdown("Region shape")),
                "Global toolbar controls incorrectly depend on the Scene sidebar tab");
        require(o.state.document.revision==tabs->revision && o.state.viewport.sequence==tabs->sequence &&
                o.state.viewport.selected_object==tabs->selected,"Tab switch changed the scene or selection");
        checkpoint(o,"01c-properties-tab");
        visible_pixels(o);
        const auto pixels=take(o.capture());
        const float scale=static_cast<float>(pixels.extent.width)/o.draw_list.logical_size.x;
        std::size_t bright{};
        for(u32 y=static_cast<u32>((o.viewport.y+20)*scale);y<static_cast<u32>((o.viewport.y+o.viewport.height-20)*scale);++y)
            for(u32 x=static_cast<u32>((o.viewport.x+20)*scale);x<static_cast<u32>((o.viewport.x+o.viewport.width-20)*scale);++x) {
                const auto offset=(static_cast<std::size_t>(y)*pixels.extent.width+x)*4;
                bright+=std::to_integer<unsigned>(pixels.pixels[offset])>90 ||
                        std::to_integer<unsigned>(pixels.pixels[offset+1])>90;
            }
        require(bright>100,"Switching sidebar tabs blanked the composited viewport");
        return true;
    });
    click(button("Keyframe values"));
    click(button("Scene"));
    add("Returning to Scene retains its widgets and selection",[this,tabs](const Observation& o) {
        const auto* heading=find({Role::label,"SCENE INSTANCES",{}});
        require(heading && heading->id==tabs->scene_heading,"Switching tabs recreated the scene page");
        require(o.state.document.revision==tabs->revision && o.state.viewport.sequence==tabs->sequence &&
                o.state.viewport.selected_object==tabs->selected,"Tab roundtrip changed document or viewport state");
        return true;
    });
    add("Scroll moves the camera by default", [this](const Observation&) {
        require(take(project::load_settings(directory_/"settings.conf")).scroll_moves_camera,"Default scroll mode is not camera translation");
        return true;
    });
    click({Role::checkbox,"Scroll moves camera",{}}); // Explicitly opt into the optical-zoom test.
    add("Wheel zoom records a target without authoring scene data", [this](const Observation& o) {
        zoom_before_=o.state.viewport.editor_camera;
        zoom_revision_=o.state.document.revision;
        zoom_sequence_=o.state.viewport.sequence;
        input::Event event;
        event.kind=input::EventKind::scroll;
        event.position=center(o.viewport); event.scroll={0,1};
        events_.push_back(event);
        return true;
    });
    add("Zoom input changes only independently sequenced viewport target", [this](const Observation& o) {
        require(o.state.document.revision==zoom_revision_ && !o.dirty,"Wheel zoom authored a document revision");
        require(o.state.viewport.sequence>zoom_sequence_,"Zoom did not advance view sequence");
        require(o.state.viewport.editor_camera.zoom>zoom_before_.zoom,"Wheel input did not update target");
        return true;
    });
    wait("Worker reaches zoom target and records first presentation separately from settling", [this](const Observation& o) {
        if (!ready(o) || !o.presented || !o.timings || o.timings->samples().empty()) return false;
        require(o.presented->has_view && o.presented->view_sequence==o.state.viewport.sequence,"Pixels have stale view metadata");
        require(o.presented->view_camera[6]==o.state.viewport.editor_camera.zoom,"Settled image did not use target camera");
        const auto& sample=o.timings->samples().back();
        if (!sample.settled_submit_ns) return false;
        require(sample.trace.presentation_submit_ns<=sample.settled_submit_ns,"Settling preceded first presentation");
        require(sample.trace.origin.estimated_input,"Synthetic input timestamp wasn't labeled estimated");
        std::ofstream(directory_/"interaction-timing.json") << o.timings->json();
        return true;
    });
    add("Restore camera by reverse wheel input", [this](const Observation& o) {
        input::Event event;
        event.kind=input::EventKind::scroll;
        event.position=center(o.viewport); event.scroll={0,-1};
        events_.push_back(event);
        return true;
    });
    wait("Restored zoom leaves document clean", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(std::abs(o.state.viewport.editor_camera.zoom-zoom_before_.zoom)<.00001F,"Reverse zoom lost original zoom");
        require(o.state.document.revision==zoom_revision_ && !o.dirty,"Viewport zoom changed save state");
        return true;
    });
    for (int step = 0; step < 12; ++step) {
        add("A fractional wheel delta changes the next UI target " + std::to_string(step),
            [this, step](const Observation& o) {
                const auto expected = zoom_before_.zoom * std::exp(.15F * static_cast<f32>(step) / 120.F);
                require(std::abs(o.state.viewport.editor_camera.zoom - expected) < .00002F,
                        "Small scroll deltas were dropped or quantized between UI ticks");
                events_.push_back({.kind = input::EventKind::scroll,
                    .position = center(o.viewport), .scroll = {0, 1.F / 120.F}});
                return true;
            });
    }
    wait("Fractional zoom reaches the real worker image", [this](const Observation& o) {
        if (!ready(o) || !o.presented) return false;
        const auto expected = zoom_before_.zoom * std::exp(.015F);
        require(std::abs(o.presented->view_camera[6] - expected) < .00002F,
                "Fractional scroll target did not reach the rendered camera");
        require(o.state.document.revision == zoom_revision_ && !o.dirty,
                "Fractional zoom changed authored data");
        events_.push_back({.kind = input::EventKind::scroll,
            .position = center(o.viewport), .scroll = {0, -.1F}});
        return true;
    });
    wait("Fractional zoom reverses without lost wheel distance", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(std::abs(o.state.viewport.editor_camera.zoom - zoom_before_.zoom) < .00002F,
                "Fractional reverse scroll lost original zoom");
        return true;
    });
    click({Role::checkbox,"Scroll moves camera",{}});
    add("Forward mode uses the same wheel gesture", [this](const Observation& o) {
        zoom_before_=o.state.viewport.editor_camera;
        events_.push_back({.kind=input::EventKind::scroll,.position=center(o.viewport),.scroll={0,1}});
        return true;
    });
    wait("Forward motion reaches worker pixels without changing the lens or document", [this](const Observation& o) {
        if (!ready(o) || !o.presented || !o.presented->settled) return false;
        const auto& pose=o.state.viewport.editor_camera;
        require(pose.target!=zoom_before_.target,"Forward scroll did not translate camera");
        require(pose.zoom==zoom_before_.zoom && pose.distance==zoom_before_.distance,"Forward scroll changed lens or orbit distance");
        require(o.presented->view_camera[6]==pose.zoom,"Worker lens differs from UI lens");
        for(unsigned i=0;i<3;++i) require(o.presented->view_camera[i+3]==pose.target[i],"Worker did not reach translated target");
        require(o.state.document.revision==zoom_revision_ && !o.dirty,"Private forward motion authored scene data");
        checkpoint(o,"02a-forward-camera");
        events_.push_back({.kind=input::EventKind::scroll,.position=center(o.viewport),.scroll={0,-1}});
        return true;
    });
    wait("Backward scroll restores position", [this](const Observation& o) {
        if (!ready(o)) return false;
        for(unsigned i=0;i<3;++i) require(std::abs(o.state.viewport.editor_camera.target[i]-zoom_before_.target[i])<.00002F,
            "Backward scroll lost original position");
        return true;
    });
    click({Role::checkbox,"Scroll moves camera",{}});
    add("Restore sidebar to its scene list", [this](const Observation&) {
        return actionable({Role::label,"SCENE INSTANCES",{}})!=nullptr;
    });
    click(button("Logs"));
    click(button("Interaction timing"));
    wait("Interaction timing panel is visible and contains measured segments", [this](const Observation& o) {
        const auto* capture=find({Role::checkbox,"Capture timing",{}});
        if (!capture) return false;
        const bool summary=std::ranges::any_of(tree_.widgets,[](const auto& w) {
            return w.visible && w.text.find("Render + readback")!=std::string::npos;
        });
        if (!summary) return false;
        checkpoint(o,"01b-interaction-timing");
        return true;
    });
    click(button("Logs"));
    scroll_workflow();
    click(button("0 s | Keyframe"));
    wait("Explicit initial keyframe selection enables scene gizmos", [](const Observation& o) {
        return ready(o) && o.selected_keyframe == 0.F && o.gizmo_visible;
    });
    add("Keyframe entries show identifiers and only selection auto-expands", [this](const Observation& o) {
        require(find(button("+", "Mesh / Mesh")) && find(button("-", "Sun / effect / Sun")),
                "Keyframe groups did not follow the selected instance");
        require(!find({Role::label,"Brightness",{}}), "Collapsed mesh still displays its fields");
        require(!o.dirty, "Expanding the selected instance changed the scene");
        checkpoint(o, "02a-collapsed-keyframe-entries");
        return true;
    });
    click(button("+", "Mesh / Mesh"));
    add("Plus reveals unselected instance fields without editing", [this](const Observation& o) {
        require(find({Role::label,"Brightness",{}}) && find(button("-", "Mesh / Mesh")),
                "Plus did not reveal the mesh keyframe fields");
        require(!o.dirty, "Expanding an entry authored scene data");
        return true;
    });
    click(button("-", "Mesh / Mesh"));
    add("Minus hides fields but keeps the identifier", [this](const Observation& o) {
        require(find(button("Mesh / Mesh")) && !find({Role::label,"Brightness",{}}),
                "Minus did not collapse the mesh keyframe fields");
        require(!o.dirty, "Collapsing an entry authored scene data");
        return true;
    });
    click(button("Mesh / Mesh"));
    wait("Keyframe identifier selects and highlights the scene instance immediately", [this](const Observation& o) {
        if (!ready(o)) return false;
        const auto* entry = find(button("Mesh / Mesh"));
        require(o.state.viewport.selected_object == 1 && std::ranges::equal(o.selected_instances, std::array<u32,1>{1}),
                "Keyframe identifier did not select the mesh in the scene");
        require(entry && entry->selected && o.gizmo_visible && find(button("-", "Mesh / Mesh")),
                "Keyframe entry, fields and gizmo did not follow scene selection");
        require(o.selected_keyframe == 0.F && !o.dirty, "Instance selection changed the keyframe or document");
        return true;
    });
    click(button("Sun / effect / Sun"));
    wait("Selecting another keyframe entry moves selection and collapses the previous entry", [this](const Observation& o) {
        if (!ready(o)) return false;
        const auto* entry = find(button("Sun / effect / Sun"));
        require(o.state.viewport.selected_object == 2 && std::ranges::equal(o.selected_instances, std::array<u32,1>{2}),
                "Keyframe selection did not reach the sun");
        require(entry && entry->selected && find(button("+", "Mesh / Mesh")),
                "Keyframe groups kept the previous selection");
        require(o.selected_keyframe == 0.F && !o.dirty, "Selection dirtied the scene");
        checkpoint(o, "02b-keyframe-instance-selection");
        return true;
    });
    click(button("Instance properties"));
    fill(field("Time"), "0");
    add("Seek zero without selecting its keyframe", [this](const Observation&) {
        key(input::Key::enter); return true;
    });
    wait("Deselecting at the same timestamp locks gizmos and property controls", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(!o.selected_keyframe && !o.gizmo_visible && !o.dirty,
                "Time zero implicitly unlocked after scrubbing");
        const auto apply = std::ranges::find_if(tree_.widgets, [](const auto& widget) {
            return widget.visible && widget.role == Role::button && widget.text == "Apply instance transform";
        });
        require(apply != tree_.widgets.end() && !apply->enabled,
                "Unselected initial keyframe left transform controls enabled");
        checkpoint(o, "02-read-only-initial-pose");
        return true;
    });
    click(button("0 s | Keyframe"));
    select_viewport("cube", [] { return 1U; });
    select_viewport("sun", [] { return 2U; });
    select_viewport("cube again", [] { return 1U; });
    add("Viewport selection highlights its keyframe identifier even on another sidebar tab", [this](const Observation& o) {
        const auto* cube = find(button("Mesh / Mesh"), false, true);
        const auto* sun = find(button("Sun / effect / Sun"), false, true);
        require(cube && cube->selected && sun && !sun->selected, "Viewport and keyframe selection diverged");
        require(o.selected_keyframe == 0.F, "Viewport selection cleared the selected keyframe");
        return true;
    });
    click(dropdown("Gizmo")); click(option("Rotate","Gizmo"));
    select_viewport("sun with rotation gizmo", [] { return 2U; });
    click(dropdown("Gizmo")); click(option("Scale","Gizmo"));
    select_viewport("cube with scale gizmo", [] { return 1U; });
    click(dropdown("Gizmo")); click(option("Move","Gizmo"));
    add("Selection screenshot", [this](const Observation& o) { checkpoint(o, "02-selected-cube"); return true; });

    click(button("Import..."));
    add("Import opens the complete authoring asset library", [this](const Observation&) {
        require(find(button("earth_savannah.vmesh", "Import / meshes and effect presets")),
                "Import did not list the new Earth asset from the source asset library");
        return true;
    });
    fill(field("Existing .vmesh / .veffect file or directory", "Import / meshes and effect presets"),
         (directory_ / "does-not-exist.vmesh").string());
    click(button("Import", "Import / meshes and effect presets"));
    wait("Invalid import is transactional", [this](const Observation& o) {
        const bool error = std::ranges::any_of(tree_.widgets, [](const auto& node) {
            return node.visible && node.text.find("does not exist") != std::string::npos;
        });
        if (!error) return false;
        require(o.modal && o.state.document.instances.size() == 2 && o.state.document.mesh_assets.empty(),
                "Invalid import changed the authored scene or closed its error dialog");
        checkpoint(o, "03-invalid-import");
        return true;
    });
    fill(field("Existing .vmesh / .veffect file or directory", "Import / meshes and effect presets"), VNG_EDITOR_SPACESHIP_PATH);
    click(button("Import", "Import / meshes and effect presets"));
    wait("Spaceship imported through file browser", [this](const Observation& o) {
        if (!ready(o) || o.state.document.instances.size() != 3 || o.state.document.mesh_assets.size() != 1) return false;
        imported_ = o.state.viewport.selected_object;
        require(imported_ != 1 && imported_ != 2 && project::is_mesh_instance(o.state, imported_),
                "Import did not create a distinct selected mesh instance");
        imported_document_ = project::instance_mesh(o.state, imported_)->document();
        imported_blueprint_ = project::find_instance(o.state, imported_)->blueprint;
        imported_view_label_ = mesh_view_label(o.state, imported_blueprint_);
        builtin_document_ = o.state.document.mesh.document();
        blueprint_projection(o, imported_blueprint_);
        visible_pixels(o);
        checkpoint(o, "04-imported-spaceship");
        return true;
    });
    click(button("Undo"));
    wait("Undo import removes its blueprint and instance", [this](const Observation& o) {
        if (!ready(o) || !o.state.document.mesh_assets.empty()) return false;
        require(o.state.document.instances.size() == 2 && !project::find_instance(o.state, imported_),
                "Undo import retained the imported instance");
        require(o.state.document.mesh.document() == *builtin_document_, "Undo import changed the cube blueprint");
        return true;
    });
    click(dropdown("View"));
    add("View menu drops undone import without stale targets", [this](const Observation& o) {
        assert_view_menu(o);
        require(!find(option(imported_view_label_, "View")), "Undone imported mesh remains selectable");
        key(input::Key::escape);
        return true;
    });
    click(button("Redo"));
    wait("Redo import restores stable asset identity and direct View destination", [this](const Observation& o) {
        if (!ready(o) || o.state.document.mesh_assets.size() != 1) return false;
        const auto* instance = project::find_instance(o.state, imported_);
        require(o.state.viewport.selected_object == imported_ && instance && instance->blueprint == imported_blueprint_,
                "Redo import replaced stable instance or blueprint IDs");
        require(mesh_view_label(o.state, imported_blueprint_) == imported_view_label_,
                "Redo import changed its View destination name");
        blueprint_projection(o, imported_blueprint_);
        return true;
    });
    click(dropdown("View"));
    click(option("Scene", "View"));
    wait("Scene retains imported ship selection before choosing cube in top View", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(o.state.viewport.mode == project::ViewMode::scene && o.state.viewport.selected_object == imported_,
                "Opening Scene changed the selected imported instance");
        return true;
    });
    choose_blueprint("builtin cube while imported instance remains selected", [] {
        return project::BlueprintId::mesh;
    }, "04a-view-cube-and-spaceship");
    wait("Top View directly targets cube independently of imported scene selection", [this](const Observation& o) {
        if (!ready(o)) return false;
        blueprint_projection(o, project::BlueprintId::mesh);
        require(o.state.viewport.selected_object == imported_, "Blueprint choice changed the scene instance selection");
        require(o.state.viewport.selected_vertex == 0, "New blueprint inspection did not select its first vertex");
        require(o.state.document.mesh.document() == *builtin_document_ &&
                    project::mesh_geometry(o.state, imported_blueprint_)->document() == *imported_document_,
                "Selecting the cube in View changed blueprint geometry");
        require(std::ranges::none_of(tree_.widgets, [](const auto& item) {
                    return item.role == Role::dropdown && item.label == "Mesh blueprint";
                }), "The old right-hand mesh selector was not removed");
        blueprint_vertex_ = o.state.document.mesh.position(0);
        visible_pixels(o);
        checkpoint(o, "04a-builtin-blueprint-selected");
        return true;
    });
    add("Delete in blueprint mode must not delete retained scene selection", [this](const Observation& o) {
        key(input::Key::del);
        input_frame_ = o.frame;
        return true;
    });
    add("Blueprint Delete leaves spaceship instance and both assets intact", [this](const Observation& o) {
        require(o.frame == input_frame_ + 1, "Blueprint Delete assertion missed its input frame");
        require(o.state.viewport.mode == project::ViewMode::mesh &&
                    o.state.viewport.inspected_mesh == project::BlueprintId::mesh &&
                    o.state.viewport.selected_object == imported_ && o.state.document.instances.size() == 3 &&
                    project::find_instance(o.state, imported_),
                "Delete in blueprint editing removed a hidden scene-instance selection");
        require(o.state.document.mesh.document() == *builtin_document_ &&
                    project::mesh_geometry(o.state, imported_blueprint_)->document() == *imported_document_,
                "Delete in blueprint mode mutated asset geometry");
        return true;
    });
    // Use the real keyboard route (not direct tool calls), then wait for the
    // worker's color image as well as the local selection/overlay response.
    for(const auto mode:{input::Key::one,input::Key::two,input::Key::three}) {
        add("Select all mesh components for H",[this,mode](const Observation& o) {
            if(!ready(o))return false;
            pointer(input::EventKind::pointer_move,center(o.viewport));key(mode);key(input::Key::a);return true;
        });
        add("Hide selected faces through H",[this](const Observation& o) {
            require(o.mesh_tools && !o.mesh_tools->selected().empty(),"Mesh selection missing before H");
            key(input::Key::h);return true;
        });
        wait("Hidden faces disappear without changing the blueprint",[this](const Observation& o) {
            if(!ready(o))return false;
            require(o.mesh_tools && o.mesh_tools->selected().empty(),"H retained hidden selection");
            require(o.mesh_tools->visibility().hidden_faces.size()==o.state.document.mesh.document().faces.size(),"H missed incident faces");
            require(o.state.document.mesh.document()==*builtin_document_,"H edited geometry");
            require(!o.state.document.mesh_drafts.contains(project::BlueprintId::mesh),"H created an authored draft");
            const auto* image=o.preview_pixels;if(!image)return false;
            std::size_t bright{};
            for(std::size_t i=0;i<image->pixels.size();i+=4)
                bright+=std::to_integer<unsigned>(image->pixels[i])>90 || std::to_integer<unsigned>(image->pixels[i+1])>90 || std::to_integer<unsigned>(image->pixels[i+2])>90;
            if(bright)return false; // wait for the matching visibility image
            key(input::Key::h,{.alt=true});return true;
        });
        wait("Alt H reveals the mesh again",[this](const Observation& o) {
            if(!ready(o))return false;
            require(o.mesh_tools && o.mesh_tools->visibility().hidden_faces.empty(),"Alt H failed to reveal");
            visible_pixels(o);return true;
        });
    }
    add("Return to vertex selection after visibility tests",[this](const Observation&) {key(input::Key::one);return true;});
    click(dropdown("View"));click(option("Scene","View"));
    choose_blueprint("cube after visibility tests",[]{return project::BlueprintId::mesh;});
    click(button("X + 0.1", "BLUEPRINT / local XYZ"));
    wait("Cube blueprint vertex edit leaves spaceship geometry unchanged", [this](const Observation& o) {
        if (!ready(o)) return false;
        blueprint_projection(o, project::BlueprintId::mesh);
        const auto vertex = project::editable_mesh(o.state)->position(0);
        require(std::abs(vertex.x - (blueprint_vertex_.x + .1F)) < .00001F &&
                    vertex.y == blueprint_vertex_.y && vertex.z == blueprint_vertex_.z,
                "Vertex nudge did not edit the inspected cube geometry");
        require(project::editable_mesh(o.state)->document() != *builtin_document_, "Cube draft geometry did not change");
        require(o.state.document.mesh.document() == *builtin_document_, "Mesh editing leaked into scene before Apply");
        require(project::mesh_geometry(o.state, imported_blueprint_)->document() == *imported_document_,
                "Editing cube blueprint unexpectedly edited the selected spaceship instance's geometry");
        require(o.state.viewport.selected_object == imported_ && o.state.document.instances.size() == 3,
                "Blueprint editing changed scene selection or created another instance");
        checkpoint(o, "04b-cube-vertex-only");
        return true;
    });
    click(dropdown("View"));
    click(option("Scene","View"));
    wait("Scene switch uses published cube, retaining unapplied draft",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.state.viewport.mode==project::ViewMode::scene,"Scene view did not open");
        require(project::mesh_view_geometry(o.state,project::BlueprintId::mesh)->document()==*builtin_document_,"Scene switch implicitly applied draft");
        require(o.state.document.mesh_drafts.contains(project::BlueprintId::mesh),"View switch lost draft");
        checkpoint(o,"mesh-draft-scene-unchanged");return true;
    });
    choose_blueprint("cube draft retained", []{return project::BlueprintId::mesh;});
    wait("Returning to mesh view keeps unsaved draft",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(std::abs(project::editable_mesh(o.state)->position(0).x-(blueprint_vertex_.x+.1F))<.00001F,"Mesh view lost working draft");return true;
    });
    click(button("Apply mesh to scene"));
    wait("Apply publishes cube without altering spaceship",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(!o.state.document.mesh_drafts.contains(project::BlueprintId::mesh),"Apply retained pending draft");
        require(std::abs(o.state.document.mesh.position(0).x-(blueprint_vertex_.x+.1F))<.00001F,"Apply did not publish vertices");
        require(project::mesh_geometry(o.state,imported_blueprint_)->document()==*imported_document_,"Apply altered another blueprint");
        visible_pixels(o);
        checkpoint(o,"mesh-draft-applied");return true;
    });
    click(button("Undo"));
    wait("Undo Apply restores unpublished draft and original scene mesh",[this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.state.document.mesh.document()==*builtin_document_,"Undo Apply did not restore scene mesh");
        require(o.state.document.mesh_drafts.contains(project::BlueprintId::mesh),"Undo Apply discarded draft");return true;
    });
    click(button("Undo"));
    wait("Undo restores only the edited cube blueprint", [this](const Observation& o) {
        if (!ready(o)) return false;
        blueprint_projection(o, project::BlueprintId::mesh);
        require(o.state.document.mesh.document() == *builtin_document_, "Undo did not restore cube vertices");
        require(project::mesh_geometry(o.state, imported_blueprint_)->document() == *imported_document_,
                "Undo changed untouched spaceship geometry");
        return true;
    });
    click({Role::checkbox,"X-ray selection",{}});
    add("Enter face selection with 3 without authoring document data", [this](const Observation& o) {
        click_at({o.viewport.x+o.viewport.width*.5F,o.viewport.y+o.viewport.height*.5F});
        key(input::Key::three); return true;
    });
    add("Select all faces using A", [this](const Observation& o) {
        require(o.mesh_tools && o.mesh_tools->mode()==project::MeshSelectMode::face,"3 did not activate face mode");
        key(input::Key::a);return true;
    });
    add("Open real mesh right-click menu", [this](const Observation& o) {
        require(o.mesh_tools->selected().size()==o.state.document.mesh.document().faces.size(),"A did not select all faces");
        const Vec2 p{o.viewport.x+o.viewport.width*.5F,o.viewport.y+o.viewport.height*.5F};
        pointer(input::EventKind::pointer_down,p,1);pointer(input::EventKind::pointer_up,p,1);return true;
    });
    add("Context menu exposes topology operations", [this](const Observation& o) {
        require(o.modal && o.mesh_tools->menu_open(),"RMB did not open mesh context menu");
        require(find(button("Subdivide"))!=nullptr,"Subdivide menu item missing");
        require(o.mesh_overlay_visible,"RMB menu disabled the mesh overlay");
        const auto item=find(button("Subdivide"))->bounds;
        require(item.x<o.viewport.x+o.viewport.width*.3F && item.y>o.viewport.y+o.viewport.height*.5F,
            "Mesh menu is not anchored at the viewport bottom left");
        checkpoint(o,"mesh-01-context-menu");return true;
    });
    click(button("Subdivide"));
    wait("Subdivide updates the actual worker and preserves the other blueprint", [this](const Observation& o) {
        if(!ready(o)) return false;
        require(project::editable_mesh(o.state)->size()>builtin_document_->vertex_count,"Subdivide added no vertices");
        require(project::editable_mesh(o.state)->document().faces.size()==builtin_document_->faces.size()*4,"Selected faces did not split into four triangles");
        require(o.state.document.mesh.document()==*builtin_document_,"Subdivision published without Apply");
        require(project::mesh_geometry(o.state,imported_blueprint_)->document()==*imported_document_,"Subdivision mutated another blueprint");
        checkpoint(o,"mesh-02-subdivided");return true;
    });
    fill(field("Subdivision levels"),"2");click(button("Update operation"));
    wait("Subdivision options recompute the original selection",[this](const Observation& o) {
        if(!ready(o))return false;
        require(project::editable_mesh(o.state)->document().faces.size()==builtin_document_->faces.size()*16,
            "Two subdivision levels did not refine the original selected patch");
        require(o.mesh_overlay_visible,"Operation options hid the overlay");
        checkpoint(o,"mesh-02-subdivision-options");return true;
    });
    fill(field("Subdivision levels"),"1");click(button("Update operation"));
    wait("Reducing subdivision levels does not stack another subdivision",[this](const Observation& o) {
        return ready(o)&&project::editable_mesh(o.state)->document().faces.size()==builtin_document_->faces.size()*4;
    });
    click(button("Undo"));
    wait("Topology Undo restores all fields and faces", [this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.state.document.mesh.document()==*builtin_document_,"Topology Undo lost original mesh data");return true;
    });
    click(button("Redo"));
    wait("Topology Redo rebuilds the subdivided preview", [this](const Observation& o) {
        if(!ready(o)) return false;
        require(project::editable_mesh(o.state)->document().faces.size()==builtin_document_->faces.size()*4,"Topology Redo failed");return true;
    });
    click(button("Undo"));
    wait("Return to original cube after topology walkthrough", [this](const Observation& o) {
        if(!ready(o)) return false;
        require(o.state.document.mesh.document()==*builtin_document_,"Final topology undo failed");
        click_at({o.viewport.x+o.viewport.width*.5F,o.viewport.y+o.viewport.height*.5F});
        key(input::Key::two);return true;
    });
    add("Edge mode has distinct component selection", [this](const Observation& o) {
        require(o.mesh_tools->mode()==project::MeshSelectMode::edge,"2 did not activate edge mode");
        key(input::Key::a);return true;
    });
    add("Edge selection expands to endpoints", [this](const Observation& o) {
        require(o.mesh_tools->edges(o.state.document.mesh).size()==o.state.document.mesh.edges().size(),"Not all edges were selected");
        key(input::Key::one);return true;
    });
    add("Vertex mode restored", [](const Observation& o) {
        require(o.mesh_tools->mode()==project::MeshSelectMode::vertex,"1 did not restore vertex mode");return true;
    });
    for(auto operation:{input::Key::g,input::Key::r,input::Key::s}) {
        auto original=std::make_shared<content::vmesh::Document>();
        add("Start a mouse-following component transform",[this,original,operation](const Observation& o) {
            if(!ready(o))return false;
            *original=project::editable_mesh(o.state)->document();
            const auto p=center(o.viewport);pointer(input::EventKind::pointer_move,{p.x+120,p.y});
            key(input::Key::a);key(operation);return true;
        });
        add("Component transform captures without holding a mouse button",[this](const Observation& o) {
            require(o.dragging,"G/R/S did not capture viewport input");
            pointer(input::EventKind::pointer_move,{pointer_.x+50,pointer_.y-40});return true;
        });
        add("Component transform updates the draft immediately",[this,original](const Observation& o) {
            require(project::editable_mesh(o.state)->document()!=*original,"Mouse-following transform did not update vertices");
            require(o.state.document.mesh.document()==*builtin_document_,"Transform published mesh without Apply");
            key(input::Key::enter);return true;
        });
        wait("Component transform releases with Enter",[this,operation](const Observation& o) {
            if(!ready(o)||o.dragging)return false;
            checkpoint(o,"mesh-modal-transform-"+std::to_string(static_cast<int>(operation)));return true;
        });
        click(button("Undo"));
        wait("Component transform is one undo entry",[original](const Observation& o) {
            if(!ready(o))return false;
            require(project::editable_mesh(o.state)->document()==*original,"Undo did not exactly restore component geometry");return true;
        });
    }
    click(dropdown("Mesh gizmo"));click(option("Move (G)","Mesh gizmo"));
    for(int operation=0;operation<2;++operation) {
        for(const u32 vertex:{0U,1U,3U}) add("Select ordered mesh vertex "+std::to_string(vertex),[this,vertex](const Observation& o) {
            if(!ready(o)) return false;
            const auto p=project::project_vertex(o.state,vertex,o.preview_extent);require(bool(p),"Mesh vertex not projected");
            const Vec2 point{o.viewport.x+p->x*o.viewport.width,o.viewport.y+p->y*o.viewport.height};
            if(vertex==0) mesh_selection_revision_=o.state.document.revision;
            pointer(input::EventKind::pointer_down,point);events_.back().modifiers.shift=vertex!=0;
            pointer(input::EventKind::pointer_up,point);events_.back().modifiers.shift=vertex!=0;
            return true;
        });
        add("Ordered Shift-selection stays outside the document",[this,operation](const Observation& o) {
            require(o.state.document.revision==mesh_selection_revision_,"Selecting mesh components authored a document change");
            mesh_selection_=o.mesh_tools->vertices(o.state.document.mesh);
            require(mesh_selection_.size()==3,"Shift-click did not retain three vertices");
            if(operation==0) key(input::Key::f);
            else {pointer(input::EventKind::pointer_down,pointer_,1);pointer(input::EventKind::pointer_up,pointer_,1);}
            return true;
        });
        if(operation==1) click(button("Align to line"));
        wait(operation==0?"F creates a face through the real input path":"Align menu keeps first two anchors fixed",[this,operation](const Observation& o) {
            if(!ready(o)) return false;
            if(operation==0) require(project::editable_mesh(o.state)->document().faces.size()==builtin_document_->faces.size()+1,"F did not create a face");
            else {
                const auto original=take(editor::EditableMesh::create(*builtin_document_));
                const auto a=project::editable_mesh(o.state)->position(mesh_selection_[0]), b=project::editable_mesh(o.state)->position(mesh_selection_[1]);
                require(a==original.position(mesh_selection_[0]) && b==original.position(mesh_selection_[1]),"Align moved an anchor");
                require(project::editable_mesh(o.state)->position(mesh_selection_[2])!=original.position(mesh_selection_[2]),"Align did not move the third vertex");
            }
            checkpoint(o,operation==0?"mesh-03-fill-face":"mesh-04-align-line");return true;
        });
        click(button("Undo"));
        wait("Mesh operation undo restores blueprint exactly",[this](const Observation& o) {
            if(!ready(o)) return false;
            require(o.state.document.mesh.document()==*builtin_document_,"Mesh operation undo did not restore all data");return true;
        });
    }
    click({Role::checkbox,"X-ray selection",{}});
    choose_blueprint("imported spaceship again", [this] { return imported_blueprint_; });
    wait("Returning to imported blueprint switches geometry and projections", [this](const Observation& o) {
        if (!ready(o)) return false;
        blueprint_projection(o, imported_blueprint_);
        require(o.state.viewport.selected_object == imported_, "Inspecting spaceship changed scene selection");
        require(project::editable_mesh(o.state)->document() == *imported_document_,
                "Returning to imported blueprint retained cube vertex data");
        require(o.mesh_tools && !o.mesh_tools->xray(), "X-ray did not switch off for solid mesh inspection");
        require(o.draw_list.commands.size()<2000, "Spaceship topology expanded into per-component UI commands again");
        visible_pixels(o);
        checkpoint(o, "04c-imported-blueprint-restored");
        return true;
    });
    click(dropdown("View"));
    click(option("Scene", "View"));
    click(dropdown("Mode"));
    click(option("Instances", "Mode"));
    add("Deselect imported ship through empty viewport", [this](const Observation& o) {
        if (!ready(o)) return false;
        const Vec2 empty{o.viewport.x + o.viewport.width * .06F, o.viewport.y + o.viewport.height * .08F};
        require(!project::pick_object(o.state, {.06F, .08F}, o.preview_extent), "Expected empty viewport corner contains geometry");
        click_at(empty);
        input_frame_ = o.frame;
        selection_dirty_ = o.dirty;
        return true;
    });
    add("Deselect removes gizmo immediately", [this](const Observation& o) {
        require(o.frame == input_frame_ + 1 && o.state.viewport.selected_object == 0 && !o.gizmo_visible,
                "Empty viewport click did not clear selection and gizmo on its input frame");
        require(o.dirty == selection_dirty_, "Deselecting changed document dirty state");
        return true;
    });
    select_viewport("imported spaceship", [this] { return imported_; });
    wait("Imported scene is ready for gizmo interaction", [this](const Observation& o) {
        if (!ready(o) || !o.inspector_ready || !o.gizmo_visible) return false;
        require(o.state.viewport.mode == project::ViewMode::scene, "Scene dropdown did not change render mode");
        original_position_ = position(o, imported_);
        original_pixels_ = o.preview_pixels->pixels;
        return true;
    });
    rotation_workflow();
    drag("Commit imported spaceship drag", false, 32);
    add("Remember committed image", [this](const Observation& o) {
        committed_position_ = position(o, imported_);
        committed_pixels_ = o.preview_pixels->pixels;
        checkpoint(o, "05-live-drag-committed");
        return true;
    });
    drag("Cancel imported spaceship drag", true, -20);
    click(button("Undo"));
    wait("Undo restores entire drag", [this](const Observation& o) {
        if (!ready(o) || position(o, imported_) != original_position_) return false;
        require(equivalent(o.preview_pixels->pixels, original_pixels_), "Undo did not restore pre-drag pixels");
        checkpoint(o, "06-undo");
        return true;
    });
    click(button("Redo"));
    wait("Redo restores committed drag", [this](const Observation& o) {
        if (!ready(o) || position(o, imported_) != committed_position_) return false;
        require(equivalent(o.preview_pixels->pixels, committed_pixels_), "Redo changed the committed image");
        return true;
    });

    click(button("Save As..."));
    fill(field("Scene path, e.g. scene.vscene", "Save scene as"), (directory_ / "edited.vscene").string());
    click(button("Save", "Save scene as"));
    wait("Save As writes private scene", [this](const Observation& o) {
        if (o.modal || o.dirty) return false;
        require(fs::is_regular_file(directory_ / "edited.vscene"), "Save As did not create the chosen scene file");
        return true;
    });
    drag("Edit before Ctrl+S", false, 12);
    add("Ctrl+S saves the current file", [this](const Observation& o) {
        saved_position_ = position(o, imported_);
        key(input::Key::s, {.control = true});
        return true;
    });
    wait("Current scene save persisted transform", [this](const Observation& o) {
        if (o.dirty || o.modal) return false;
        const auto saved = take(project::load_scene(directory_ / "edited.vscene"));
        require(project::instance_transform(saved, imported_)->position == saved_position_,
                "Ctrl+S did not save the current authored position");
        return true;
    });
    drag("Unsaved edit before Load", false, -12);
    click(button("Open scene"));
    wait("Load restores the saved scene", [this](const Observation& o) {
        if (!ready(o) || o.dirty || position(o, imported_) != saved_position_) return false;
        require(o.state.document.mesh_assets.size() == 1, "Load lost the imported mesh blueprint");
        animation_camera_ = o.state.document.animation_camera;
        return true;
    });
    click(dropdown("View"));
    add("Loaded scene refreshes all named View destinations", [this](const Observation& o) {
        assert_view_menu(o);
        require(find(option(mesh_view_label(o.state, project::BlueprintId::mesh), "View")) &&
                    find(option(mesh_view_label(o.state, imported_blueprint_), "View")),
                "Loaded cube or imported mesh is missing from top View");
        key(input::Key::escape);
        return true;
    });
    fill(field("Orbit distance"), "20");
    add("Commit numeric editor-camera distance", [this](const Observation&) {
        key(input::Key::enter);
        return true;
    });
    wait("Editor camera changes independently of saved animation shot", [this](const Observation& o) {
        if (!ready(o) || o.state.viewport.editor_camera.distance != 20) return false;
        require(o.state.document.animation_camera == animation_camera_, "Editor camera field changed the animation shot");
        require(!o.dirty, "Private editor navigation dirtied the saved scene");
        visible_pixels(o);
        checkpoint(o, "07-reloaded-scene-camera-navigation");
        return true;
    });

    click({Role::checkbox, "Edit animation camera", {}});
    wait("Animation camera pilot ready", [](const Observation& o) {
        return ready(o) && o.state.viewport.pilot_camera;
    });
    add("Reloading clears keyframe authoring permission", [](const Observation& o) {
        require(!o.selected_keyframe && !o.gizmo_visible && !o.dirty,
                "Reloading retained an editable keyframe");
        return true;
    });
    click(button("0 s | Keyframe"));
    enum class CameraAbort { escape, focus, minimize, overflow };
    for (const auto abort : {CameraAbort::escape, CameraAbort::focus, CameraAbort::minimize, CameraAbort::overflow}) {
        const auto reason = abort == CameraAbort::escape ? "Escape" : abort == CameraAbort::focus ? "focus loss" :
                            abort == CameraAbort::minimize ? "minimization" : "input overflow";
        add(std::string{"Start authored camera drag before "} + reason,
            [this](const Observation& o) {
                animation_camera_ = o.state.document.animation_camera;
                drag_start_ = center(o.viewport);
                pointer(input::EventKind::pointer_down, drag_start_, 2);
                return true;
            });
        add("Move animation camera while held", [this](const Observation&) {
            pointer(input::EventKind::pointer_move, {drag_start_.x + 35, drag_start_.y + 10}, 2);
            return true;
        });
        add("Camera gesture updates locally and gates unrelated controls", [this, abort](const Observation& o) {
            require(o.state.document.animation_camera != animation_camera_ && o.dirty,
                    "Authored camera gesture did not preview locally");
            const auto add_key = std::ranges::find_if(tree_.widgets, [](const auto& widget) {
                return widget.role == Role::button && widget.text == "Add keyframe";
            });
            require(add_key != tree_.widgets.end() && !add_key->enabled,
                    "Camera capture left timeline editing enabled");
            switch (abort) {
            case CameraAbort::escape: key(input::Key::escape); break;
            case CameraAbort::focus: events_.push_back({.kind=input::EventKind::focus_lost}); break;
            case CameraAbort::minimize: minimize_next_input_ = true; break;
            case CameraAbort::overflow: overflow_next_input_ = true; break;
            }
            return true;
        });
        wait("Camera cancellation restores authored shot and saved identity", [this](const Observation& o) {
            if (!ready(o)) return false;
            require(o.state.document.animation_camera == animation_camera_, "Cancelled camera gesture committed its pose");
            require(!o.dirty, "Cancelled camera gesture changed saved-content identity");
            pointer(input::EventKind::pointer_up, pointer_, 2);
            events_.push_back({.kind=input::EventKind::focus_gained});
            return true;
        });
    }
    click({Role::checkbox, "Edit animation camera", {}});
    wait("Private camera restored after cancelled authoring", [](const Observation& o) {
        return ready(o) && !o.state.viewport.pilot_camera;
    });

    instance_modal_workflow();
    click(button("Instance properties"));
    wait("Instance transform inspector ready", [this](const Observation& o) {
        if (!ready(o) || !o.inspector_ready) return false;
        before_scale_pixels_ = o.preview_pixels->pixels;
        require(std::ranges::any_of(tree_.widgets, [](const auto& widget) {
            return widget.visible && widget.text.starts_with("Blueprint: ");
        }), "Instance list/inspector does not expose the referenced blueprint");
        return true;
    });
    fill(field("Scale", "Instance transform"), "0.55");
    click(button("Apply instance transform"));
    wait("Typed scale Apply changes the scene, not the blueprint", [this](const Observation& o) {
        if (!ready(o) || project::instance_transform(o.state, imported_)->scale != .55F) return false;
        require(!equivalent(before_scale_pixels_, o.preview_pixels->pixels),
                "Accepted scale Apply did not change actual scene pixels");
        require(project::instance_mesh(o.state, imported_)->document() == *imported_document_,
                "Instance scaling rewrote shared mesh geometry");
        scaled_pixels_ = o.preview_pixels->pixels;
        checkpoint(o, "08a-instance-scale");
        return true;
    });
    convenience_workflow();
    fill(field("Z", "Rotation (degrees)"), "35");
    click(button("Apply instance transform"));
    wait("Rotation Apply updates scene placement", [this](const Observation& o) {
        if (!ready(o) || project::instance_transform(o.state, imported_)->rotation.z != 35) return false;
        require(!equivalent(scaled_pixels_, o.preview_pixels->pixels), "Rotation did not change rendered pixels");
        require(project::instance_mesh(o.state, imported_)->document() == *imported_document_,
                "Instance rotation changed blueprint vertices");
        scaled_pixels_ = o.preview_pixels->pixels;
        checkpoint(o, "08b-instance-rotation");
        return true;
    });
    forward_workflow();
    attitude_workflow();
    fill(field("Time"),"3");
    add("Seek unkeyed time before insertion",[this](const Observation&){key(input::Key::enter);return true;});
    click(button("Instance properties"));
    wait("Between-key scene pose is read-only", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(!project::at_paused_keyframe(o.state), "Unkeyed pose remained editable");
        const auto apply = std::ranges::find_if(tree_.widgets, [](const auto& widget) {
            return widget.visible && widget.role == Role::button && widget.text == "Apply instance transform";
        });
        require(apply != tree_.widgets.end() && !apply->enabled,
                "Unkeyed instance transform controls remained enabled");
        checkpoint(o,"08-read-only-pose");
        return true;
    });
    click(button("Add keyframe"));
    wait("Timeline keyframe is selected", [](const Observation& o) {
        return o.selected_keyframe.has_value() && o.state.document.keyframe_names.contains(*o.selected_keyframe);
    });
    add("Keyframe inspector screenshot", [this](const Observation& o) { checkpoint(o, "08-keyframe"); return true; });
    const auto before_range = std::make_shared<timeline::Timeline>();
    add("Draft a keyframe scale for range application", [this, before_range](const Observation& o) {
        *before_range = o.state.document.timeline;
        const auto* label = find({Role::label, "Scale", {}});
        require(label != nullptr, "Selected instance has no scale row");
        const auto value = std::ranges::find_if(tree_.widgets, [&](const auto& widget) {
            const auto* parent = node(widget.parent);
            return widget.visible && widget.enabled && widget.role == Role::text_field && parent && parent->parent == label->parent;
        });
        require(value != tree_.widgets.end(), "Scale row has no editable value");
        click_at(center(intersection(value->bounds, value->clip)));
        key(input::Key::a, {.control = true});
        events_.push_back({.kind = input::EventKind::text, .text = "0.9"});
        return true;
    });
    click(button("Apply to keyframes"));
    fill(field("From (s)"), "0");
    add("Range menu exposes destination count without changing the scene", [this, before_range](const Observation& o) {
        require(o.state.document.timeline == *before_range, "Opening the range menu applied a draft early");
        require(find({Role::label, "2 keyframes in range", {}}) != nullptr, "Range menu has the wrong destination count");
        checkpoint(o, "08a-keyframe-range-menu");
        return true;
    });
    click(button("Apply range"));
    wait("Toolbar range edit reaches every destination and preserves other tracks", [this, before_range](const Observation& o) {
        if (!ready(o)) return false;
        const timeline::Target target{imported_, "scale"};
        for (auto time : {0.F, o.state.viewport.time}) {
            const auto value = o.state.document.timeline.sample(target, time);
            require(value && std::get<f32>(*value) == .9F, "Range did not apply the edited scale at both timestamps");
        }
        for (const auto& track : before_range->tracks()) if (track.target != target) {
            const auto* after = o.state.document.timeline.find(track.target);
            require(after && *after == track, "Range edit overwrote an unrelated property");
        }
        require(!find(button("Apply range")), "Successful range edit did not close its menu");
        return true;
    });
    click(button("Undo"));
    wait("One Undo restores the entire keyframe range", [before_range](const Observation& o) {
        return ready(o) && o.state.document.timeline == *before_range;
    });
    click(button("Instance properties"));
    wait("Animated scale inspector ready", [](const Observation& o) { return ready(o) && o.inspector_ready; });
    fill(field("Scale", "Instance transform"), "0.8");
    click(button("Apply instance transform"));
    wait("Apply edits an animated scale at the current playhead", [this](const Observation& o) {
        if (!ready(o)) return false;
        const auto* instance = project::find_instance(o.state, imported_);
        const auto evaluated = project::evaluate_instance(o.state, *instance, o.state.viewport.time);
        if (evaluated.transform.scale != .8F) return false;
        require(instance->transform.scale == .55F, "Animated Apply rewrote base scale instead of the key");
        require(evaluated.transform.rotation.z == 35, "Scale Apply changed an untouched rotation field");
        require(!equivalent(scaled_pixels_, o.preview_pixels->pixels),
                "A timeline key still masked the scale Apply in rendered output");
        require(project::instance_mesh(o.state, imported_)->document() == *imported_document_,
                "Animated scale mutated the shared blueprint");
        checkpoint(o, "08c-animated-scale-apply");
        key(input::Key::s, {.control = true});
        return true;
    });
    wait("Save persists separated instance transform and animation", [this](const Observation& o) {
        if (o.dirty) return false;
        const auto saved = take(project::load_scene(directory_ / "edited.vscene"));
        const auto* instance = project::find_instance(saved, imported_);
        require(instance && instance->transform.scale == .55F && instance->transform.rotation.z == 35,
                "Scene save lost base instance transform");
        require(project::evaluate_instance(saved, *instance, saved.viewport.time).transform.scale == .8F,
                "Scene save lost applied scale key");
        return true;
    });
    click(button("Keyframe values"));
    click(button("Delete keyframe"));
    wait("Timeline keyframe deleted", [](const Observation& o) {
        // Creating a later key also seeds baseline keys at zero. Deleting the
        // selected timestamp must retain those independent baseline keys.
        return !o.selected_keyframe && o.state.document.keyframe_names.empty() &&
               std::ranges::none_of(o.state.document.timeline.tracks(), [&](const auto& track) {
                   return std::ranges::any_of(track.keys, [&](const auto& key) { return key.time == o.state.viewport.time; });
               });
    });
    fill(field("Time"), "0");
    add("Return to editable initial pose", [this](const Observation&) {
        key(input::Key::enter); return true;
    });
    click(button("0 s | Keyframe"));

    add("Instantiate the same spaceship blueprint again", [this](const Observation& o) {
        if (!ready(o)) return false;
        const auto blueprint = project::find_instance(o.state, imported_)->blueprint;
        const auto catalog = project::blueprint_catalog(o.state);
        const auto entry = std::ranges::find(catalog, blueprint, &project::Blueprint::id);
        require(entry != catalog.end(), "Imported blueprint disappeared before sibling creation");
        const auto* target = actionable(button("+", "Edit " + std::string(entry->name)));
        if (!target) return false;
        click_at(center(intersection(target->bounds, target->clip)));
        return true;
    });
    wait("Second instance has an independent identity transform", [this](const Observation& o) {
        if (!ready(o) || o.state.document.instances.size() != 4) return false;
        sibling_ = o.state.viewport.selected_object;
        const auto* sibling = project::find_instance(o.state, sibling_);
        const auto* first = project::find_instance(o.state, imported_);
        require(sibling && sibling_ != imported_ && sibling->blueprint == first->blueprint,
                "Blueprint action did not create a sibling instance");
        require(sibling->transform == project::InstanceTransform{},
                "New instance inherited another instance's scale/rotation/position");
        require(first->transform.scale == .55F && first->transform.rotation.z == 35,
                "Creating a sibling changed the existing instance transform");
        require(project::instance_mesh(o.state, sibling_) == project::instance_mesh(o.state, imported_) &&
                    project::instance_mesh(o.state, imported_)->document() == *imported_document_,
                "Sibling instantiation copied or changed shared blueprint geometry");
        checkpoint(o, "08d-two-instances-one-blueprint");
        key(input::Key::del);
        return true;
    });
    wait("Remove sibling without removing original or blueprint", [this](const Observation& o) {
        return ready(o) && !project::find_instance(o.state, sibling_) &&
               project::find_instance(o.state, imported_) && o.state.document.mesh_assets.size() == 1;
    });
    select_viewport("original spaceship after deleting sibling", [this] { return imported_; });

    add("Remember production image", [this](const Observation& o) {
        if (!ready(o)) return false;
        normal_pixels_ = o.preview_pixels->pixels;
        return true;
    });
    click({Role::checkbox, "Face IDs", {}});
    wait("Enhanced face-ID render", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(!equivalent(o.preview_pixels->pixels, normal_pixels_), "Face IDs did not change actual worker output");
        checkpoint(o, "09-face-ids");
        return true;
    });
    click({Role::checkbox, "Face IDs", {}});
    wait("Normal output restored", [this](const Observation& o) {
        return ready(o) && equivalent(o.preview_pixels->pixels, normal_pixels_);
    });
    click(button("Settings"));
    fill(field("Embedded preview FPS cap", "EDITOR SETTINGS"), "45");
    fill(field("Debug-preview FPS cap (0 = unlimited)", "EDITOR SETTINGS"), "144");
    click({Role::checkbox, "VSync (editor and independent Play)", "EDITOR SETTINGS"});
    add("Settings screenshot", [this](const Observation& o) { checkpoint(o, "10-settings"); return true; });
    click(button("Apply & save", "EDITOR SETTINGS"));
    wait("Settings saved privately", [this](const Observation& o) {
        if (o.modal) return false;
        const auto settings = take(project::load_settings(directory_ / "settings.conf"));
        require(settings.preview_fps == 45,
                "Settings UI did not persist its numeric field");
        require(settings.debug_fps == 144, "Debug-preview field still enforces the old 60 FPS ceiling");
        require(settings.vsync == window::VSync::off, "VSync checkbox did not persist");
        return true;
    });
    click(button("Settings"));
    fill(field("Debug-preview FPS cap (0 = unlimited)", "EDITOR SETTINGS"), "0");
    click({Role::checkbox, "VSync (editor and independent Play)", "EDITOR SETTINGS"});
    click(button("Apply & save", "EDITOR SETTINGS"));
    wait("Unlimited debug-preview rate persists through actual Settings UI", [this](const Observation& o) {
        if (o.modal) return false;
        const auto settings = take(project::load_settings(directory_ / "settings.conf"));
        require(settings.debug_fps == 0 && settings.preview_fps == 45,
                "Unlimited debug-preview rate failed to persist or changed another cap");
        require(settings.vsync == window::VSync::on, "VSync checkbox did not retain/reapply its preference");
        return true;
    });
    click(button("Add region"));
    wait("Region appears without entering mesh view", [this](const Observation& o) {
        if(!ready(o))return false;
        require(project::region_snapshot(o.state).items.size()==1,"Add region did not create an annotation");
        require(o.state.viewport.mode==project::ViewMode::scene,"Region switched into blueprint editing");
        require(o.region_gizmo && o.region_gizmo->selected(),"New region has no scene handles");
        checkpoint(o,"10b-region-created");return true;
    });
    instance_modal_workflow();
    fill(field("Region name"),"Arrival zone");
    fill({Role::text_area,"Region TODO note",{}},"TODO: add a refueling station here");
    click(button("Apply region"));
    wait("Region has its own filtered instance row",[this](const Observation& o) {
        if(!ready(o))return false;
        const auto* regions=find({Role::label,"REGION INSTANCES",{}},false,true);
        const auto* meshes=find({Role::label,"SCENE INSTANCES",{}},false,true);
        require(regions&&meshes,"Missing separate instance lists");
        const auto row=std::ranges::find_if(tree_.widgets,[&](const auto& w) {
            return w.role==Role::button && w.text.ends_with("Arrival zone") && inside(w,regions->parent);
        });
        require(row!=tree_.widgets.end(),"Region is absent from its dedicated list");
        require(!inside(*row,meshes->parent),"Region also appears in the mesh instance list");
        drag_pixels_=o.preview_pixels->pixels;return true;
    });
    click({Role::checkbox,"Show walls (grid)",{}});
    wait("Wall grid changes the actual worker image",[this](const Observation& o) {
        if(!ready(o))return false;
        require(project::region_snapshot(o.state).items.front().show_walls,"Wall checkbox did not update the instance");
        require(!equivalent(drag_pixels_,o.preview_pixels->pixels),"Wall grid did not reach the preview renderer");
        checkpoint(o,"10b-region-wall-grid");key(input::Key::z,{.control=true});return true;
    });
    wait("Wall setting Undo restores edges only",[this](const Observation& o) {
        if(!ready(o))return false;
        require(!project::region_snapshot(o.state).items.front().show_walls,"Undo did not restore wall setting");
        require(equivalent(drag_pixels_,o.preview_pixels->pixels),"Undo did not restore preview pixels");
        key(input::Key::y,{.control=true});return true;
    });
    wait("Wall setting Redo restores grid",[this](const Observation& o) {
        return ready(o)&&project::region_snapshot(o.state).items.front().show_walls;
    });
    click(dropdown("Gizmo"));click(option("Region vertices","Gizmo"));
    wait("Region metadata applied",[this](const Observation& o) {
        if(!ready(o))return false;
        require(project::region_snapshot(o.state).items.front().name=="Arrival zone","Region name did not apply");
        require(std::ranges::any_of(tree_.widgets,[](const auto& w){return w.role==Role::button&&w.text.ends_with("Arrival zone");}),
            "Renaming region did not update its ordinary instance row");
        const auto r=project::region_snapshot(o.state).items.front();region_vertex_=r.points.front();
        const auto p=o.region_gizmo->point_handle(r.id,0);require(p.has_value(),"Region vertex is not visible");
        click_at(*p);return true;
    });
    add("Drag a region vertex in scene",[this](const Observation& o) {
        require(o.region_gizmo->point()==0,"Click did not select region vertex");
        const auto p=o.region_gizmo->handle("X");require(p.has_value(),"Region vertex has no XYZ gizmo");
        pointer(input::EventKind::pointer_down,*p);pointer(input::EventKind::pointer_move,{p->x+30,p->y});
        pointer(input::EventKind::pointer_up,{p->x+30,p->y});return true;
    });
    wait("Region deformation commits locally and reaches worker",[this](const Observation& o) {
        if(!ready(o))return false;
        const auto p=project::region_snapshot(o.state).items.front().points.front();
        require(p.x!=region_vertex_.x && p.y==region_vertex_.y && p.z==region_vertex_.z,"Region drag failed or changed another axis");
        checkpoint(o,"10c-region-deformed");return true;
    });
    click(button("Next vertex"));
    wait("Next vertex keeps the same region instance",[](const Observation& o) {
        if(!ready(o))return false;
        require(o.region_gizmo->point()==1,"Next vertex did not select vertex one");
        require(o.selected_instances.size()==1&&o.selected_instances.front()==o.region_gizmo->selected(),
            "Vertex navigation changed scene instance selection");return true;
    });
    click(button("Previous vertex"));
    wait("Previous vertex returns to vertex zero",[](const Observation& o) {
        return ready(o)&&o.region_gizmo->point()==0;
    });
    click(button("Scene"));
    for(auto operation:{input::Key::g,input::Key::r,input::Key::s}) {
        auto before=std::make_shared<project::Region>();
        add("Select all region boundary points",[this,before](const Observation& o) {
            if(!ready(o))return false;
            *before=project::region_snapshot(o.state).items.front();
            const auto p=center(o.viewport);pointer(input::EventKind::pointer_move,{p.x+100,p.y});
            key(input::Key::a);return true;
        });
        add("Start region boundary G R S",[this,operation](const Observation&) {
            key(operation);return true;
        });
        add("Boundary modal capture owns input",[this](const Observation& o) {
            require(o.dragging,"Region boundary did not start G/R/S");
            pointer(input::EventKind::pointer_move,{pointer_.x+40,pointer_.y-30});return true;
        });
        add("Region boundary previews all selected points immediately",[this,before](const Observation& o) {
            require(project::region_snapshot(o.state).items.front().points!=before->points,"Region points did not follow the mouse");
            key(input::Key::enter);return true;
        });
        wait("Boundary gesture confirms",[](const Observation& o){return ready(o)&&!o.dragging;});
        click(button("Undo"));
        wait("Boundary gesture is one undo entry",[before](const Observation& o) {
            if(!ready(o))return false;
            require(project::region_snapshot(o.state).items.front().points==before->points,"Undo did not restore every boundary point");
            return true;
        });
    }
    click(dropdown("Boundary gizmo"));click(option("Move (G)","Boundary gizmo"));
    add("Restore single region vertex selection",[this](const Observation& o) {
        const auto r=project::region_snapshot(o.state).items.front();
        const auto p=o.region_gizmo->point_handle(r.id,0);require(p.has_value(),"Missing region vertex after modal transforms");
        click_at(*p);return true;
    });
    const auto region_menu=[this] {
        add("Open selected region RMB menu",[this](const Observation& o) {
            if(!ready(o))return false;
            require(o.region_gizmo&&o.region_gizmo->selected(),"Region was deselected before RMB");
            const Vec2 p{o.viewport.x+o.viewport.width-20,o.viewport.y+20};
            pointer(input::EventKind::pointer_down,p,1);pointer(input::EventKind::pointer_up,p,1);return true;
        });
    };
    region_menu();
    wait("Selected region menu is visible",[this](const Observation& o) {
        if(!o.modal)return false;
        checkpoint(o,"10c-region-rmb-menu");return true;
    });
    click(button("Select faces"));
    region_menu();click(button("Select all components"));
    region_menu();click(button("Subdivide region selection"));
    wait("Region RMB subdivision preserves polygon connectivity",[this](const Observation& o) {
        if(!ready(o))return false;
        const auto r=project::region_snapshot(o.state).items.front();
        require(r.points.size()==26&&r.faces.size()==24,"Face subdivision did not share boundary midpoints");
        require(o.region_gizmo->elements().size()==18,"New subdivision vertices were not selected");
        checkpoint(o,"10d-region-subdivided");return true;
    });
    region_menu();click(button("Delete selected components"));
    wait("Region component deletion permits open authoring state",[this](const Observation& o) {
        if(!ready(o))return false;
        const auto r=project::region_snapshot(o.state).items.front();
        require(r.points.size()==8&&r.faces.empty(),"Delete components lost remapping or failed");
        key(input::Key::z,{.control=true});return true;
    });
    wait("Region topology Undo restores faces",[](const Observation& o) {
        return ready(o)&&project::region_snapshot(o.state).items.front().faces.size()==24;
    });
    region_menu();click(button("Add vertex"));
    wait("RMB adds a selected region vertex",[](const Observation& o) {
        if(!ready(o))return false;
        require(project::region_snapshot(o.state).items.front().points.size()==27,"Add vertex failed");
        require(o.region_gizmo->elements().size()==1,"Added vertex not selected");return true;
    });
    region_menu();click(button("Delete selected components"));
    wait("RMB removes only the added vertex",[this](const Observation& o) {
        if(!ready(o))return false;
        const auto r=project::region_snapshot(o.state).items.front();
        require(r.points.size()==26&&r.faces.size()==24,"Loose vertex deletion changed faces");
        key(input::Key::s,{.control=true});return true;
    });
    wait("Region survives native scene persistence",[this](const Observation& o) {
        if(o.dirty)return false;
        const auto saved=take(project::load_scene(directory_/"edited.vscene"));
        require(project::region_snapshot(saved)==project::region_snapshot(o.state),"Save lost region data");
        key(input::Key::del);return true;
    });
    wait("Delete removes region, not the selected ship",[this](const Observation& o) {
        if(!ready(o))return false;
        require(project::region_snapshot(o.state).items.empty(),"Region Delete failed");
        require(project::find_instance(o.state,imported_)!=nullptr,"Deleting region deleted ship instead");
        key(input::Key::z,{.control=true});return true;
    });
    wait("Undo restores region",[this](const Observation& o) {
        if(!ready(o))return false;
        require(project::region_snapshot(o.state).items.size()==1,"Undo lost region");
        key(input::Key::y,{.control=true});return true;
    });
    wait("Redo deletes only region",[](const Observation& o) {return ready(o)&&project::region_snapshot(o.state).items.empty();});
    add("RMB without selected region has no region menu",[this](const Observation& o) {
        const Vec2 p{o.viewport.x+o.viewport.width-20,o.viewport.y+20};
        pointer(input::EventKind::pointer_down,p,1);pointer(input::EventKind::pointer_up,p,1);return true;
    });
    add("No stray region menu",[](const Observation& o) {require(!o.modal,"Region menu appeared without a region");return true;});
    select_viewport("imported ship after region deletion",[this]{return imported_;});
    click(button("Instance properties"));
    add("Delete imported instance with keyboard", [this](const Observation& o) {
        if (!ready(o)) return false;
        blueprints_ = o.state.document.mesh_assets.size();
        key(input::Key::del);
        return true;
    });
    wait("Deleting instance retains reusable blueprint", [this](const Observation& o) {
        if (!ready(o) || project::find_instance(o.state, imported_)) return false;
        require(o.state.document.mesh_assets.size() == blueprints_, "Deleting a scene instance deleted its mesh asset");
        scene_selection_before_blueprint_ = o.state.viewport.selected_object;
        checkpoint(o, "11-deleted-instance-retained-blueprint");
        return true;
    });
    choose_blueprint("orphan spaceship with no remaining instances", [this] { return imported_blueprint_; });
    wait("Orphan blueprint renders and projects vertices without a scene instance", [this](const Observation& o) {
        if (!ready(o)) return false;
        blueprint_projection(o, imported_blueprint_);
        require(o.state.document.instances.size() == 2 &&
                    std::ranges::none_of(o.state.document.instances, [this](const auto& instance) {
                        return instance.blueprint == imported_blueprint_;
                    }), "Opening a retained blueprint silently instantiated it");
        require(o.state.viewport.selected_object == scene_selection_before_blueprint_,
                "Orphan blueprint inspection overwrote scene selection");
        require(project::editable_mesh(o.state)->document() == *imported_document_,
                "Deleting all instances discarded the blueprint's geometry");
        require(o.state.viewport.selected_vertex == 0, "Orphan blueprint has an invalid initial selected vertex");
        blueprint_vertex_ = project::editable_mesh(o.state)->position(0);
        visible_pixels(o);
        checkpoint(o, "12-orphan-blueprint");
        return true;
    });
    click(button("X + 0.1", "BLUEPRINT / local XYZ"));
    wait("Orphan blueprint vertex tools edit only retained asset geometry", [this](const Observation& o) {
        if (!ready(o)) return false;
        blueprint_projection(o, imported_blueprint_);
        const auto vertex = project::editable_mesh(o.state)->position(0);
        require(std::abs(vertex.x - (blueprint_vertex_.x + .1F)) < .00001F &&
                    vertex.y == blueprint_vertex_.y && vertex.z == blueprint_vertex_.z,
                "Vertex edit requires a live scene instance instead of the inspected blueprint");
        require(project::editable_mesh(o.state)->document() != *imported_document_ &&
                    o.state.document.mesh.document() == *builtin_document_,
                "Orphan edit changed the wrong geometry");
        require(o.state.document.instances.size() == 2 && o.state.document.mesh_assets.size() == blueprints_,
                "Editing orphan asset changed instance or blueprint counts");
        checkpoint(o, "13-orphan-vertex-edited");
        return true;
    });
    click(button("Undo"));
    wait("Undo restores orphan geometry without resurrecting deleted instances", [this](const Observation& o) {
        if (!ready(o)) return false;
        blueprint_projection(o, imported_blueprint_);
        require(project::editable_mesh(o.state)->document() == *imported_document_ &&
                    o.state.document.mesh.document() == *builtin_document_, "Undo restored the wrong blueprint");
        require(o.state.document.instances.size() == 2 && !project::find_instance(o.state, imported_) &&
                    !project::find_instance(o.state, sibling_), "Undo of an asset edit resurrected deleted instances");
        checkpoint(o, "14-orphan-vertex-undo");
        return true;
    });
    click(dropdown("Mode"));
    click(option("Instances", "Mode"));
    // Asset inspection and scene instance tools must never share a hidden target.
    wait("Instance interaction mode returns to Scene without fabricating a selection", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(o.state.viewport.mode == project::ViewMode::scene,
                "Instances interaction mode left an isolated blueprint on screen");
        require(o.state.viewport.selected_object == scene_selection_before_blueprint_ &&
                    o.state.viewport.selected_object == 0 && o.state.document.instances.size() == 2,
                "Switching to Scene selected or created an invisible instance");
        require(o.state.viewport.inspected_mesh == imported_blueprint_, "Scene mode forgot the inspected orphan blueprint");
        require(!project::mesh_target(o.state) && !project::editable_mesh(o.state),
                "Empty Scene selection silently retained a blueprint vertex-edit target");
        return true;
    });
    choose_blueprint("retained orphan after returning to Scene", [this] { return imported_blueprint_; });
    wait("Direct View entry opens orphan asset after Scene interaction mode", [this](const Observation& o) {
        if (!ready(o)) return false;
        blueprint_projection(o, imported_blueprint_);
        require(o.state.viewport.selected_object == scene_selection_before_blueprint_ && o.state.document.instances.size() == 2,
                "Returning to remembered blueprint altered scene instances or selection");
        require(project::editable_mesh(o.state)->document() == *imported_document_,
                "Returning to remembered blueprint displayed different geometry");
        visible_pixels(o);
        checkpoint(o, "15-orphan-blueprint-remembered");
        return true;
    });
    click(dropdown("View")); click(option("Scene", "View"));
    click(button("Settings"));
    fill(field("Minimum orbit distance", "EDITOR SETTINGS"), "0.005");
    fill(field("Maximum orbit distance", "EDITOR SETTINGS"), "25000");
    fill(field("Maximum viewing distance", "EDITOR SETTINGS"), "50000");
    fill(field("Walk forward speed (units/s)", "EDITOR SETTINGS"), "15");
    fill(field("Walk sideways speed (units/s)", "EDITOR SETTINGS"), "8");
    fill(field("Walk vertical speed (units/s)", "EDITOR SETTINGS"), "6");
    fill(field("Walk Shift multiplier", "EDITOR SETTINGS"), "3");
    click(button("Apply & save", "EDITOR SETTINGS"));
    wait("Expanded zoom preferences persist", [this](const Observation& o) {
        if(o.modal) return false;
        const auto settings=take(project::load_settings(directory_/"settings.conf"));
        require(settings.orbit_distance==project::OrbitDistanceRange{.005F,25000},"Zoom preferences did not persist");
        require(settings.maximum_viewing_distance==50000 && settings.walk==project::WalkSpeeds{15,8,6,3},"Camera settings did not persist");
        if(!o.presented || o.presented->view_far_plane!=50000) return false;
        return true;
    });
    click(button("Frame world bounds"));
    wait("World bounds frame and six handles appear beyond the old zoom limit", [this](const Observation& o) {
        if(!ready(o) || !o.bounds_gizmo || !o.bounds_gizmo->handle(1)) return false;
        require(o.state.viewport.editor_camera.distance>100,"Framing still stops at old zoom ceiling");
        bounds_before_=o.state.document.world_bounds;
        checkpoint(o,"16-world-bounds");
        drag_start_=*o.bounds_gizmo->handle(1);
        events_.push_back({.kind=input::EventKind::pointer_down,.position=drag_start_});
        return true;
    });
    add("Drag world positive X face", [this](const Observation& o) {
        require(o.bounds_gizmo->dragging(),"Bounds handle failed to capture");
        events_.push_back({.kind=input::EventKind::pointer_move,.position={drag_start_.x+30,drag_start_.y}});
        return true;
    });
    add("Bounds preview changes only its selected face immediately", [this](const Observation& o) {
        require(o.state.document.world_bounds.maximum.x!=bounds_before_.maximum.x,"Bounds drag has no immediate feedback");
        require(o.state.document.world_bounds.minimum==bounds_before_.minimum,"Bounds drag moved the opposite faces");
        events_.push_back({.kind=input::EventKind::pointer_up,.position={drag_start_.x+30,drag_start_.y}});
        return true;
    });
    wait("World bounds drag commits and reaches worker", [this](const Observation& o) {
        if(!ready(o)) return false;
        require(!o.bounds_gizmo->dragging(),"Bounds remained captured after release");
        bounds_after_=o.state.document.world_bounds; checkpoint(o,"17-world-bounds-resized"); return true;
    });
    click(button("Undo"));
    wait("Undo restores the entire bounds gesture", [this](const Observation& o) {
        if(!ready(o))return false;
        require(o.state.document.world_bounds==bounds_before_,"Bounds undo did not restore original corners");return true;
    });
    click(button("Redo"));
    wait("Redo restores resized bounds", [this](const Observation& o) {
        if(!ready(o))return false;
        require(o.state.document.world_bounds==bounds_after_,"Bounds redo lost corners");return true;
    });
    fill(field("World minimum X"), "-125");
    click(button("Apply world bounds"));
    wait("Numeric world bounds controls edit the same cuboid", [this](const Observation& o) {
        if(!ready(o))return false;
        auto expected=bounds_after_; expected.minimum.x=-125;
        require(o.state.document.world_bounds==expected,"Numeric bounds Apply changed the wrong corners");
        bounds_after_=expected; checkpoint(o,"18-world-bounds-coordinates");return true;
    });
    click(button("Save"));
    wait("Scene save contains world bounds", [this](const Observation& o) {
        if(o.dirty)return false;
        auto loaded=take(project::load_scene(directory_/"edited.vscene"));
        require(loaded.document.world_bounds==bounds_after_,"Saved scene lost world bounds");return true;
    });
    add("Begin another bounds drag for cancellation", [this](const Observation& o) {
        if(!ready(o) || !o.bounds_gizmo->handle(1))return false;
        drag_start_=*o.bounds_gizmo->handle(1);
        events_.push_back({.kind=input::EventKind::pointer_down,.position=drag_start_});return true;
    });
    add("Preview a cancellable bounds change", [this](const Observation& o) {
        require(o.bounds_gizmo->dragging(),"Bounds drag not captured");
        events_.push_back({.kind=input::EventKind::pointer_move,.position={drag_start_.x+20,drag_start_.y}});return true;
    });
    add("Escape cancels boundary drag", [this](const Observation& o) {
        require(o.dirty,"Bounds preview did not mark document changed"); key(input::Key::escape); return true;
    });
    wait("Bounds cancellation restores saved values and identity", [this](const Observation& o) {
        if(!ready(o))return false;
        require(!o.dirty && o.state.document.world_bounds==bounds_after_,"Cancel failed to restore saved bounds");return true;
    });
    click(button("Walk camera"));
    add("Walk W key starts camera-relative movement", [this](const Observation& o) {
        zoom_before_=o.state.viewport.editor_camera; zoom_revision_=o.state.document.revision;
        events_.push_back({.kind=input::EventKind::key_down,.key=input::Key::w}); return true;
    });
    add("Walk moves immediately without changing zoom or scene", [this](const Observation& o) {
        const auto& pose=o.state.viewport.editor_camera;
        require(pose.target!=zoom_before_.target,"Walk button did not enable W movement");
        require(pose.zoom==zoom_before_.zoom && pose.distance==zoom_before_.distance,"Walk altered lens or orbit distance");
        require(o.state.document.revision==zoom_revision_ && !o.dirty,"Private walk authored the scene");
        events_.push_back({.kind=input::EventKind::key_up,.key=input::Key::w});
        zoom_before_=pose; return true;
    });
    wait("Released walking keys stop and reach worker pixels", [this](const Observation& o) {
        require(o.state.viewport.editor_camera==zoom_before_,"Released W kept walking");
        if(!ready(o) || !o.presented || !o.presented->settled) return false;
        for(unsigned i=0;i<3;++i) require(o.presented->view_camera[i+3]==zoom_before_.target[i],"Walk pose did not reach worker");
        checkpoint(o,"19-camera-walk"); return true;
    });
    fill(field("Zoom"), "2");
    add("Typing movement letters in a numeric field cannot move the camera", [this](const Observation&) {
        events_.push_back({.kind=input::EventKind::key_down,.key=input::Key::w}); return true;
    });
    add("Focused fields suppress walk input", [this](const Observation& o) {
        require(o.state.viewport.editor_camera==zoom_before_,"Typing W moved camera");
        events_.push_back({.kind=input::EventKind::key_up,.key=input::Key::w});
        key(input::Key::escape); return true;
    });
    wait("Escape exits walk mode", [this](const Observation&) { return actionable(button("Walk camera"))!=nullptr; });
    fill(field("Maximum viewing distance","CAMERA SETTINGS"),"60000");
    click(button("Save camera preferences"));
    wait("Flyout preferences reach worker without changing document",[this](const Observation& o) {
        if(!o.presented || o.presented->view_far_plane!=60000) return false;
        require(!o.dirty,"Camera preferences dirtied scene");
        checkpoint(o,"20-camera-flyout"); return true;
    });
    click(button("Settings"));
    fill(field("UI scale %","EDITOR SETTINGS"),"125");
    click(button("Apply & save","EDITOR SETTINGS"));
    wait("UI scale enlarges widgets without downsampling the framebuffer",[this](const Observation& o) {
        if(o.modal || !ready(o)) return false;
        const auto settings=take(project::load_settings(directory_/"settings.conf"));
        require(settings.ui_scale_percent==125,"UI scale was not persisted");
        if(std::abs(o.draw_list.logical_size.x*1.25F*project::editor_ui_density-static_cast<float>(o.extent.width))>=1) return false;
        require(o.draw_list.framebuffer==o.extent,"UI scale reduced framebuffer resolution");
        const auto* settings_button=find(button("Settings"));
        require(settings_button && intersection(settings_button->bounds,settings_button->clip).width>=settings_button->bounds.width-1,
                "Scaled toolbar hid Settings");
        checkpoint(o,"21-ui-scale-125"); return true;
    });
    click(button("Camera settings"));
    wait("Scaled camera flyout stays below its toolbar button and within the window",[this](const Observation& o) {
        const auto* close=actionable(button("Close camera settings"));
        if(!close) return false;
        const auto* opener=find(button("Camera settings"));
        require(opener && close->bounds.y>=opener->bounds.y+opener->bounds.height,"Camera flyout is not below its toolbar button");
        require(close->bounds.x>=0 && close->bounds.x+close->bounds.width<=o.draw_list.logical_size.x,
                "Camera flyout extends outside the window");
        checkpoint(o,"22-scaled-camera-flyout"); return true;
    });
    add("Orbit while camera settings stay open",[this](const Observation& o) {
        zoom_before_=o.state.viewport.editor_camera;
        const Vec2 p{o.viewport.x+o.viewport.width*.8F,o.viewport.y+o.viewport.height*.8F};
        events_.push_back({.kind=input::EventKind::pointer_down,.position=p,.button=2});
        events_.push_back({.kind=input::EventKind::pointer_move,.position={p.x+24,p.y+8},.button=2});
        events_.push_back({.kind=input::EventKind::pointer_up,.position={p.x+24,p.y+8},.button=2});
        return true;
    });
    add("Navigation does not dismiss camera settings",[this](const Observation& o) {
        require(find(button("Close camera settings")),"Camera navigation dismissed camera settings");
        require(o.state.viewport.editor_camera!=zoom_before_,"Camera menu blocked viewport orbit");
        return true;
    });
    // Real multi-frame drags: clicking or filling the adjacent field cannot
    // detect capture being cancelled by the application's value synchronization.
    for (const auto label : {"Orbit", "Elevation", "Orbit distance", "Zoom"}) {
        auto expected = std::make_shared<f32>();
        const Locator locator{Role::slider,label,"CAMERA SETTINGS"};
        const auto sample = [this,locator,expected](int index, input::EventKind kind) {
            const auto* slider = find(locator);
            require(slider && slider->minimum && slider->maximum,"Missing camera slider");
            const float fraction = .20F + static_cast<float>(index)*.015F;
            const auto inset = ui::ThemeValues{}.padding;
            const Vec2 p{slider->bounds.x+inset+(slider->bounds.width-2*inset)*fraction,
                         slider->bounds.y+slider->bounds.height*.5F};
            *expected = *slider->minimum + fraction*(*slider->maximum-*slider->minimum);
            events_.push_back({.kind=kind,.position=p});
        };
        add(std::string("Begin continuous slider drag: ")+label,[sample](const Observation&) {
            sample(0,input::EventKind::pointer_down); return true;
        });
        for(int i=0;i<8;++i) add(std::string("Slider follows input frame: ")+label+" / "+std::to_string(i),
            [this,locator,label,expected,sample,i](const Observation& o) {
                const auto* slider=find(locator);
                require(slider && slider->pressed && slider->number,"Slider lost capture during synchronization");
                const auto& camera=o.state.viewport.editor_camera;
                const auto value=std::string_view(label)=="Orbit" ? camera.yaw :
                    std::string_view(label)=="Elevation" ? camera.pitch :
                    std::string_view(label)=="Zoom" ? camera.zoom : camera.distance;
                const auto tolerance=std::max(.001F,std::abs(*expected)*.0001F);
                require(std::abs(*slider->number-*expected)<tolerance,"Slider thumb lags pointer");
                require(std::abs(value-*expected)<tolerance,"Camera value lags slider input");
                sample(i+1,i==7 ? input::EventKind::pointer_up : input::EventKind::pointer_move);
                return true;
            });
        add(std::string("Slider releases normally: ")+label,[this,locator](const Observation&) {
            const auto* slider=find(locator);
            require(slider && !slider->pressed,"Slider did not release"); return true;
        });
    }
    click(button("Close camera settings"));
    click(button("UI +"));
    wait("Toolbar UI plus scales and saves immediately",[this](const Observation& o) {
        if(std::abs(o.draw_list.logical_size.x*1.30F*project::editor_ui_density-static_cast<float>(o.extent.width))>=1) return false;
        require(take(project::load_settings(directory_/"settings.conf")).ui_scale_percent==130,"Toolbar scale was not saved");
        require(!o.dirty,"UI scaling dirtied the scene"); return true;
    });
    click(button("UI -"));
    wait("Toolbar UI minus remains clickable after scaling",[](const Observation& o) {
        return std::abs(o.draw_list.logical_size.x*1.25F*project::editor_ui_density-static_cast<float>(o.extent.width))<1;
    });
    click(button("Settings"));
    fill(field("UI scale %","EDITOR SETTINGS"),"150");
    click(button("Apply & save","EDITOR SETTINGS"));
    wait("Maximum UI scale keeps primary actions in row one and exposes scene overflow",[this](const Observation& o) {
        if(o.modal || std::abs(o.draw_list.logical_size.x*1.5F*project::editor_ui_density-static_cast<float>(o.extent.width))>=1) return false;
        for(const auto label:{"Open scene","Import...","Logs","Settings","More...","Tools..."}) {
            const auto* action=find(button(label));
            require(action && intersection(action->bounds,action->clip).width>=action->bounds.width-1,
                    "Missing or clipped toolbar action at 150%: "+std::string(label));
        }
        require(!find(dropdown("Region shape")),"Scene controls should move to overflow at 150%");
        return true;
    });
    click(button("Tools..."));
    click(dropdown("Region shape"));
    click(option("Triangular prism","Region shape"));
    add("Overflow region shape changes only the creation choice",[this](const Observation& o) {
        const auto* shape=find(dropdown("Region shape"),false,true);
        require(shape && shape->text=="Triangular prism","Overflow dropdown did not accept selection");
        require(!o.dirty,"Choosing a region template changed the document");
        key(input::Key::escape);
        return true;
    });
    click(button("More..."));
    add("Application overflow retains UI scale controls at maximum scale",[this](const Observation& o) {
        const auto* scale=find(button("UI -"));
        require(scale && intersection(scale->bounds,scale->clip).width>=scale->bounds.width-1,
                "Application overflow clipped UI scale controls");
        checkpoint(o,"23-toolbar-overflow-150");
        key(input::Key::escape);
        return true;
    });
    click(button("Scene"));
    click(button("Instance list"));
    wait("Instance flyout opens while the right sidebar retains both lists", [this](const Observation& o) {
        const auto* close = find(button("Close instance list"));
        if (!close) return false;
        const auto* title = find({Role::label,"INSTANCE LIST",{}});
        const auto* panel = title ? node(title->parent) : nullptr;
        const auto* tools = find(button("Tools..."));
        require(tools && panel && panel->bounds.y >= tools->bounds.y + tools->bounds.height,
                "List flyout lost its toolbar anchor after closing overflow");
        require(panel && panel->bounds.x >= 0 && panel->bounds.y >= 0 &&
                panel->bounds.x + panel->bounds.width <= o.draw_list.logical_size.x &&
                panel->bounds.y + panel->bounds.height <= o.draw_list.logical_size.y,
                "Instance flyout extends beyond the scaled window");
        require(find({Role::label,"SCENE INSTANCES",{}},false) && find({Role::label,"BLUEPRINTS",{}},false),
                "Popping out lists removed the sidebar lists");
        scroll_revision_ = o.state.document.revision;
        checkpoint(o,"24-instance-list-flyout");
        return true;
    });
    add("Select the cube through the floating instance list", [this](const Observation& o) {
        const auto* instance = project::find_instance(o.state, 1); require(instance,"No cube instance");
        const auto prefix = o.state.viewport.selected_object == 1 ? "> #" : "#";
        const auto* row = actionable(button(std::string(prefix)+"1 "+instance->name,"INSTANCE LIST"));
        if (!row) return false;
        click_at(center(intersection(row->bounds,row->clip))); return true;
    });
    wait("Floating list selection reaches the scene and retained sidebar list", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(o.state.viewport.selected_object == 1, "Floating list did not select the cube");
        const auto label = "> #1 " + project::find_instance(o.state,1)->name;
        require(find(button(label,"INSTANCE LIST")) && find(button(label,"SCENE INSTANCES"),false),
                "Docked and floating instance lists disagree");
        require(o.state.document.revision == scroll_revision_, "Opening or selecting a list edited scene data");
        return true;
    });
    click(button("Close instance list"));
    click(button("Blueprint list"));
    wait("Blueprint flyout shares the library with the sidebar", [this](const Observation& o) {
        const auto* edit = find(button("Edit Mesh","BLUEPRINT LIST"));
        if (!edit) return false;
        require(find(button("Edit Mesh","BLUEPRINTS"),false), "Docked blueprint list disappeared");
        require(intersection(edit->bounds,edit->clip).width >= edit->bounds.width-1,
                "Blueprint flyout clips its edit control");
        const auto* plus = node(edit->parent);
        require(plus, "Missing blueprint row");
        for (const auto& widget : tree_.widgets) if (widget.parent==plus->id && widget.text=="+")
            require(widget.visible && widget.clip.width==widget.bounds.width, "Blueprint add button is clipped");
        const auto* heading = find({Role::label,"BLUEPRINT LIST",{}}); require(heading,"Missing blueprint flyout");
        for (const auto& widget : tree_.widgets)
            if (widget.role==Role::button && widget.text=="+" && inside(widget,heading->parent))
                require(widget.visible && widget.clip.width==widget.bounds.width,
                        "A long blueprint name pushed its add button outside the list");
        checkpoint(o,"25-blueprint-list-flyout");
        return true;
    });
    click(button("Edit Mesh","BLUEPRINT LIST"));
    wait("Floating blueprint edit opens the correct blueprint without altering the scene", [this](const Observation& o) {
        if (!ready(o)) return false;
        require(o.state.viewport.mode == project::ViewMode::mesh &&
                o.state.viewport.inspected_mesh == project::BlueprintId::mesh,
                "Floating blueprint action opened the wrong mesh");
        require(o.state.document.revision == scroll_revision_, "Blueprint inspection changed the scene");
        key(input::Key::escape);
        return true;
    });
    add("Escape dismisses the list flyout", [this](const Observation&) {
        require(!find(button("Close blueprint list")), "Escape did not close blueprint flyout"); return true;
    });
    click(button("Settings"));
    fill(field("UI scale %","EDITOR SETTINGS"),"100");
    click(button("Apply & save","EDITOR SETTINGS"));
    click(button("Import..."));
    fill(field("Existing .vmesh / .veffect file or directory", "Import / meshes and effect presets"),
         (fs::path(VNG_EDITOR_SPACESHIP_PATH).parent_path()/"quiet_sun.veffect").string());
    click(button("Import", "Import / meshes and effect presets"));
    wait("Effect preset imports as an independent selected blueprint", [this](const Observation& o) {
        if(!ready(o) || o.state.document.effect_assets.empty())return false;
        const auto* instance=project::find_instance(o.state,o.state.viewport.selected_object);
        require(instance && instance->blueprint==o.state.document.effect_assets.back().id,"Preset import selected the wrong instance");
        require(o.state.viewport.mode==project::ViewMode::sun,"Preset did not open effect inspection");
        require(std::get<project::SunSettings>(instance->settings)==project::SunSettings{1.3F,.4F,.12F,false,true},"Imported defaults changed");
        visible_pixels(o); checkpoint(o,"26-imported-effect-preset"); return true;
    });
    click(button("Export new .veffect"));
    wait("Selected effect exports a reusable preset without changing the scene target", [this](const Observation& o) {
        if(!fs::exists(directory_/"edited.veffect"))return false;
        const auto preset=take(project::read_effect_preset(directory_/"edited.veffect"));
        require(preset==take(project::effect_preset(o.state,o.state.viewport.selected_object)),"Export lost the selected instance's effect settings");
        return !o.modal;
    });
    click(button("Save"));
    wait("Scene save embeds imported presets", [this](const Observation& o) {
        if(o.dirty)return false;
        const auto saved=take(project::load_scene(directory_/"edited.vscene"));
        require(saved.document.effect_assets==o.state.document.effect_assets,"Scene save lost effect blueprints"); return true;
    });
    click(button("Undo"));
    wait("Undo effect import removes its blueprint and instance together", [](const Observation& o) {
        return ready(o) && o.state.document.effect_assets.empty();
    });
    click(button("Redo"));
    wait("Redo effect import restores the same blueprint", [this](const Observation& o) {
        if(!ready(o) || o.state.document.effect_assets.empty())return false;
        require(!o.dirty,"Redo did not restore saved effect scene identity");
        visible_pixels(o); checkpoint(o,"27-restored-effect-preset"); return true;
    });
}
} // namespace

int main(int argc, char** argv) {
    try {
        fs::path root = VNG_EDITOR_E2E_ARTIFACTS_DIR;
        bool fleet{},multi{},popout{},earth{};
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--artifacts" && i + 1 < argc) root = argv[++i];
            else if (std::string_view(argv[i]) == "--fleet") fleet = true;
            else if (std::string_view(argv[i]) == "--multi") multi = true;
            else if (std::string_view(argv[i]) == "--popout") popout = true;
            else if (std::string_view(argv[i]) == "--earth") earth = true;
            else throw std::runtime_error("Usage: vng_editor_e2e_tests [--artifacts DIR] [--fleet | --multi | --popout | --earth]");
        }
        fs::create_directories(root);
        auto pattern = (fs::absolute(root) / "run-XXXXXX").string();
        auto* created = ::mkdtemp(pattern.data());
        require(created != nullptr, "Cannot create private E2E artifact directory");
        const fs::path directory{created};
        std::cout << "E2E artifacts: " << directory << '\n';
        take(project::save_settings(directory / "settings.conf", project::Settings{}));
        Driver driver{directory, fleet, multi, popout,earth};
        try {
            project::Options options;
            options.windowed = true;
            // The shipped scenes are also users' editable projects. Never
            // depend on (or overwrite) their saved edits in a walkthrough:
            // author a private fixture from the generator instead.
            const auto author_fixture=[&](auto&& author,const char* name) {
                auto fixture=take(author());
                project::SceneFile file;
                options.scene=directory/name;
                take(file.save_as(*options.scene,fixture));
            };
            if (fleet) author_fixture([]{return example::asteroids::author_scene(fs::path(VNG_EDITOR_FLEET_PATH).parent_path());},"fleet-fixture.vscene");
            if (earth) author_fixture([]{return example::earth::author_scene(fs::path(VNG_EDITOR_EARTH_PATH).parent_path());},"earth-fixture.vscene");
            options.automation = &driver;
            options.preferences = directory / "settings.conf";
            const auto result = project::run(options);
            if (result == 0) require(driver.finished(), "Editor exited before the automation suite completed");
            if (result == 77) driver.skipped("Initial OpenGL context is unavailable");
            else if (result != 0) driver.startup_failure("Editor returned " + std::to_string(result));
            return result;
        } catch (const std::exception& error) {
            driver.startup_failure(error.what());
            std::cerr << "E2E failed: " << error.what() << "\nArtifacts: " << directory << '\n';
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "E2E setup failed: " << error.what() << '\n';
        return 1;
    }
}
