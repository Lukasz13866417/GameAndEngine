#include "region_editor.hpp"
#include <charconv>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace editor_example {
namespace {
using namespace vng;
std::optional<float> number(std::string_view text) {
    const auto first=text.find_first_not_of(" \t");if(first==text.npos)return {};
    text=text.substr(first,text.find_last_not_of(" \t")-first+1);
    float v{};auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),v);
    if(error!=std::errc{}||end!=text.data()+text.size()||!std::isfinite(v))return {};
    return v;
}
std::string number(float v) {std::array<char,64> b{};auto [end,e]=std::to_chars(b.data(),b.data()+b.size(),v);return e==std::errc{}?std::string(b.data(),end):"";}
void place(Region& r,std::span<const u32> vertices,Vec3 value) {
    Vec3 center{};
    if(vertices.empty())center=r.center();
    else for(auto v:vertices)for(unsigned a=0;a<3;++a)center[a]+=r.points.at(v)[a]/static_cast<float>(vertices.size());
    if(vertices.empty())for(auto& p:r.points)for(unsigned a=0;a<3;++a)p[a]+=value[a]-center[a];
    else for(auto v:vertices)for(unsigned a=0;a<3;++a)r.points.at(v)[a]+=value[a]-center[a];
}
std::vector<u32> moved_points(const CageTool& tool, const Region& region) {
    auto result = tool.vertices();
    if (result.empty()) {
        result.resize(region.points.size());
        std::iota(result.begin(), result.end(), 0U);
    }
    return result;
}
std::vector<RegionPointEdit> point_values(const State& state, const Region& world, std::span<const u32> ids) {
    Region subset; subset.id = world.id;
    for (auto id : ids) subset.points.push_back(world.points.at(id));
    const auto local = region_to_local(state, subset);
    std::vector<RegionPointEdit> result;
    result.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) result.push_back({ids[i], local.points[i]});
    return result;
}
}
RegionEditor::RegionEditor(ui::Container controls,ui::Container creation,ui::Container inspector,ui::Container popup)
    :controls_(controls.row().height(36).padding(0).gap(4)),host_(creation.column().padding(0).gap(5)),
    details_(inspector.column().padding(0).gap(6).visible(false)),popup_(popup),
    show_(controls_.checkbox("Regions / TODO volumes").width(280).value(true)),
    shape_(controls_.dropdown<RegionShape>("Region shape",{{RegionShape::box,"Box"},{RegionShape::tetrahedron,"Tetrahedron"},
        {RegionShape::octahedron,"Octahedron"},{RegionShape::prism,"Triangular prism"}}).width(320).value(RegionShape::box)),
    transform_(details_.dropdown<GizmoMode>("Boundary gizmo",{{GizmoMode::move,gizmo_choice_label(GizmoMode::move)},
        {GizmoMode::rotate,gizmo_choice_label(GizmoMode::rotate)},{GizmoMode::scale,gizmo_choice_label(GizmoMode::scale)},
        {GizmoMode::free_rotate,gizmo_choice_label(GizmoMode::free_rotate)}})) {
    host_.label("New region radius").height(22);
    radius_=host_.text_input("New region radius").value("2");
    add_=host_.button("Add region");
    // Region identity stays in SceneInstance; its dedicated list is a filtered view.
    details_.label("REGION / scene annotation").height(26);
    details_.label("Name").height(22);
    name_=details_.text_input("Region name");
    details_.label("TODO note").height(22);
    note_=details_.text_area("Region TODO note").height(100);
    walls_=details_.checkbox("Show walls (grid)");
    point_=details_.label("Select a region vertex").height(24);
    auto coordinates=details_.row().height(38).padding(0).gap(4);
    for(unsigned a=0;a<3;++a) {
        coordinates.label(std::string(1,"XYZ"[a])).width(20);
        xyz_[a]=coordinates.text_input(std::string{"Region "}+"XYZ"[a]);
    }
    auto vertex_navigation=details_.row().height(36).padding(0).gap(4);
    previous_vertex_=vertex_navigation.button("Previous vertex");next_vertex_=vertex_navigation.button("Next vertex");
    apply_=details_.button("Apply region");remove_=details_.button("Delete region");deselect_=details_.button("Deselect region");
    details_.label("RMB in scene: region tools").height(24);
    details_.label("Shift / Ctrl-click: extend selection").height(24);
    popup_.width(290).height(468).padding(8).gap(4).scrollbar(ui::ScrollBar::automatic).visible(false);
    popup_.label("Region tools").height(24);
    modes_[0]=popup_.button("Select vertices").height(32);
    modes_[1]=popup_.button("Select edges").height(32);
    modes_[2]=popup_.button("Select faces").height(32);
    all_=popup_.button("Select all components").height(32);
    clear_=popup_.button("Clear component selection").height(32);
    add_vertex_=popup_.button("Add vertex").height(32);
    fill_=popup_.button("Make edge / face").height(32);
    subdivide_=popup_.button("Subdivide region selection").height(32);
    align_=popup_.button("Align to line").height(32);
    erase_elements_=popup_.button("Delete selected components").height(32);
    erase_region_=popup_.button("Delete entire region").height(32);
}
void RegionEditor::selection(u32 object,GizmoMode mode) {
    const bool object_changed = tool_.selected()!=object;
    if(object_changed)tool_.select(object,region_center);
    const auto component = region_component_mode(mode) ? static_cast<editor::CageElement>(
        static_cast<int>(mode)-static_cast<int>(GizmoMode::region_vertices)) : editor::CageElement::vertex;
    if(gizmo_!=mode || object_changed || tool_.mode()!=component) {
        gizmo_=mode;
        tool_.mode(component);
    }
    tool_.components(region_component_mode(mode));
}
void RegionEditor::close_menu() {menu_open_=false;popup_.visible(false);}
void RegionEditor::open_menu(Vec2 at,Vec2 screen,std::optional<ui::Rect> viewport) {
    if(!selected()||!editable_||dragging())return;
    const auto height=std::min(468.F,std::max(80.F,screen.y-16));
    if(viewport)at={viewport->x+8,viewport->y+std::max(0.F,viewport->height-height-8)};
    popup_.height(height).position({std::clamp(at.x,0.F,std::max(0.F,screen.x-290)),
        std::clamp(at.y,0.F,std::max(0.F,screen.y-height))}).visible(true);
    const auto vertices=tool_.vertices();
    fill_.enabled(tool_.mode()==editor::CageElement::vertex&&vertices.size()>=2);
    subdivide_.enabled(!tool_.elements().empty());align_.enabled(vertices.size()>=3);
    erase_elements_.enabled(!tool_.elements().empty());menu_open_=true;
}
void RegionEditor::geometry(EditingSession& editing,RegionAction action) {
    const auto current=region_world_snapshot(editing.state(),static_cast<u32>(tool_.selected()));
    if(!current)return;
    auto candidate=*current;
    auto changed=edit_region_geometry(candidate,action,tool_.mode(),tool_.elements());
    if(!changed){message_=changed.error().message;return;}
    const auto ids = moved_points(tool_, *current);
    const bool points_only = action == RegionAction::align;
    auto begun=points_only ? editing.begin_region_points(candidate.id, ids) : editing.begin_region(candidate.id);
    if(!begun){message_=begun.error().message;return;}
    auto applied=points_only ? editing.region_points(point_values(editing.state(), candidate, ids))
                            : editing.region(region_to_local(editing.state(),candidate));
    if(!applied){(void)editing.cancel();message_=applied.error().message;return;}
    auto committed=editing.commit();
    if(!committed){message_=committed.error().message;return;}
    if (points_only) pending_.regions[candidate.id].points.insert(ids.begin(), ids.end());
    else pending_.regions[candidate.id].whole = true;
    sync(editing.state());
    requested_mode_=GizmoMode::region_vertices;gizmo_=*requested_mode_;tool_.components(true);
    tool_.select_elements(editor::CageElement::vertex,*changed);
    sync(editing.state());
    message_="Region geometry updated / Undo restores it";
}
void RegionEditor::poll_menu(EditingSession& editing,std::span<const input::Event> events) {
    if(!menu_open_)return;
    if(!selected()||editing.busy()||!editing.state().viewport.paused){close_menu();return;}
    for(unsigned i=0;i<3;++i)if(modes_[i].clicked()) {
        requested_mode_=static_cast<GizmoMode>(static_cast<int>(GizmoMode::region_vertices)+i);
        selection(static_cast<u32>(tool_.selected()),*requested_mode_);close_menu();return;
    }
    if(all_.clicked()){tool_.select_all();close_menu();return;}
    if(clear_.clicked()){tool_.select_elements(tool_.mode(),{});close_menu();return;}
    std::optional<RegionAction> action;
    if(add_vertex_.clicked())action=RegionAction::add_vertex;
    if(fill_.clicked())action=RegionAction::fill;
    if(subdivide_.clicked())action=RegionAction::subdivide;
    if(align_.clicked())action=RegionAction::align;
    if(erase_elements_.clicked())action=RegionAction::erase;
    if(action){close_menu();geometry(editing,*action);return;}
    if(erase_region_.clicked()) {
        close_menu();auto removed=erase(editing);
        if(!removed)message_=removed.error().message;else if(*removed)message_="Deleted region / Undo restores it";
        return;
    }
    for(const auto& e:events)if(e.kind==input::EventKind::focus_lost||
        (e.kind==input::EventKind::key_down&&e.key==input::Key::escape)||
        (e.kind==input::EventKind::pointer_down&&!popup_.bounds().contains(e.position)))close_menu();
}
void RegionEditor::sync_inspector(const Regions& regions) {
    const auto* r=find_region(regions,static_cast<u32>(tool_.selected()));
    if(!r){close_menu();tool_.select(0,0);details_.visible(false);displayed_.reset();return;}
    if(tool_.point()!=region_center&&tool_.point()>=r->points.size())tool_.select(r->id,region_center);
    details_.visible(true);
    if(!displayed_||*displayed_!=*r||displayed_point_!=tool_.point()||displayed_selection_!=tool_.selection_revision()) {
        const bool changed_region=!displayed_||displayed_->id!=r->id;
        if(changed_region||displayed_->name!=r->name)name_.value(r->name);
        if(changed_region||displayed_->note!=r->note)note_.value(r->note);
        if(changed_region||displayed_->show_walls!=r->show_walls)walls_.value(r->show_walls);
        displayed_=*r;displayed_point_=tool_.point();displayed_selection_=tool_.selection_revision();
        const auto p=tool_.pivot().value_or(r->center());
        const auto count=tool_.elements().size();
        const auto kind=tool_.mode()==editor::CageElement::vertex?" vertices":tool_.mode()==editor::CageElement::edge?" edges":" faces";
        point_.text(region_component_mode(gizmo_)
            ? std::to_string(count)+kind+(count?" selected / world XYZ":" selected / entire boundary")
            : "Instance: use Move / Rotate / Scale");
        for(unsigned a=0;a<3;++a)xyz_[a].value(number(p[a]));
    }
}
void RegionEditor::sync(const State& state) {
    const auto selected = static_cast<u32>(tool_.selected());
    const auto reset_topology = [&](const Region* old, const Region* next) {
        if (old && next && (old->points.size() != next->points.size() || old->faces != next->faces ||
                           old->loose_edges != next->loose_edges)) {
            const auto mode = tool_.mode();
            tool_.select(selected, region_center);
            tool_.mode(mode);
        }
    };
    // Explicit edit notices avoid revisiting unrelated boundaries. Time changes
    // and structural edits legitimately resample the entire visible cage set.
    if (!cached_ || pending_.full || cached_time_ != state.viewport.time ||
        (cached_revision_ != state.document.revision && pending_.empty())) {
        auto next = region_world_snapshot(state);
        reset_topology(cached_ ? find_region(*cached_, selected) : nullptr, find_region(next, selected));
        cached_ = std::move(next);
        std::vector<editor::SceneCage> cages;
        for (const auto& r : cached_->items) cages.push_back(r.scene_cage());
        tool_.refresh(cages);
    } else if (!pending_.empty()) {
        auto objects = pending_.regions;
        for (const auto& property : pending_.properties)
            if (property.object <= UINT32_MAX && region_settings(state, static_cast<u32>(property.object)))
                objects[static_cast<u32>(property.object)].whole = true;
        for (const auto& [id, scope] : objects) {
            auto old = std::ranges::find(cached_->items, id, &Region::id);
            const auto* settings = region_settings(state, id);
            if (!scope.whole && old != cached_->items.end() && settings &&
                old->points.size() == settings->boundary.points.size()) {
                Region subset; subset.id = id;
                for (auto point : scope.points) subset.points.push_back(settings->boundary.points.at(point));
                const auto world = region_to_world(state, subset);
                std::vector<editor::ScenePoint> positions;
                positions.reserve(scope.points.size()+1);
                std::size_t index{};
                for (auto point : scope.points) {
                    old->points[point] = world.points[index++];
                    positions.push_back({point,old->points[point],{}});
                }
                positions.push_back({region_center,old->center(),{}});
                tool_.refresh_points(id, positions);
                continue;
            }
            auto next = region_world_snapshot(state, id);
            if (id == selected) reset_topology(old == cached_->items.end() ? nullptr : &*old, next ? &*next : nullptr);
            if (next) {
                tool_.refresh(next->scene_cage());
                if (old == cached_->items.end()) cached_->items.push_back(std::move(*next));
                else *old = std::move(*next);
            } else {
                tool_.erase(id);
                if (old != cached_->items.end()) cached_->items.erase(old);
            }
        }
    }
    cached_revision_ = state.document.revision; cached_time_ = state.viewport.time;
    pending_ = {};
    sync_inspector(*cached_);
}
content::Result<bool> RegionEditor::erase(EditingSession& editing) {
    auto result=editing.erase_region(static_cast<u32>(tool_.selected()));
    if(result&&*result){pending_.full=true;close_menu();tool_.clear();displayed_.reset();selection_=0;}return result;
}
void RegionEditor::update(EditingSession& editing,const gfx::CameraSnapshot& camera,ui::Rect viewport,
    std::span<const input::Event> input,std::span<const input::Event> raw,bool visible,bool editable,float arrow_step) {
    visible_=visible;editable_=editable;host_.visible(visible);host_.enabled(editable&&!editing.busy());
    controls_.enabled(visible && editable && !editing.busy());
    details_.enabled(editable&&!editing.busy());
    for(auto field:xyz_)field.enabled(component_editing());
    transform_.visible(component_editing());
    if(auto mode=transform_.changedValue())tool_.transform_mode(*mode);
    previous_vertex_.enabled(component_editing()&&gizmo_==GizmoMode::region_vertices);
    next_vertex_.enabled(component_editing()&&gizmo_==GizmoMode::region_vertices);
    shape_.enabled(show_.value());radius_.enabled(show_.value());add_.enabled(show_.value());
    if(!visible||!show_.value()) {
        if(editing.active(EditGesture::region)) {auto cancelled=editing.cancel();if(!cancelled)message_=cancelled.error().message;}
        close_menu();tool_.hide();details_.visible(false);return;
    }
    sync(editing.state());
    if(editable&&!editing.busy()) {
        if (auto walls = walls_.changedValue())
            if (const auto* current = find_region(*cached_, static_cast<u32>(tool_.selected()))) {
                auto next = *current; next.show_walls = *walls;
                // Changing display style must not round-trip geometry through
                // the instance transform (and accumulate floating-point drift).
                static_cast<RegionGeometry&>(next) = region_settings(editing.state(),next.id)->boundary;
                auto begun = editing.begin_region(next.id);
                if (!begun) message_ = begun.error().message;
                else if (auto changed = editing.region(next); !changed) {
                    message_ = changed.error().message; (void)editing.cancel();
                } else {
                    auto committed = editing.commit();
                    if (!committed) message_ = committed.error().message;
                    pending_.regions[next.id].whole = true;
                }
                sync(editing.state());
            }
        if(add_.clicked()) {
            const auto radius=number(radius_.getText());
            if(!radius||*radius<=0)message_="Region radius must be a positive finite number";
            else {
                auto p=camera.position;for(unsigned a=0;a<3;++a)p[a]+=camera.forward[a] * *radius * 6;
                auto created=editing.add_region(shape_.value(),p,*radius);
                if(!created)message_=created.error().message;
                else {pending_.full=true;selection_=*created;selection(*created,GizmoMode::move);message_="Added region instance / use Gizmo to edit its boundary";}
            }
        }
        if(deselect_.clicked()){tool_.select(0,0);selection_=0;}
        if(remove_.clicked()) {auto removed=erase(editing);message_=removed?"Deleted region / Undo restores it":removed.error().message;}
        const auto& world=*cached_;
        if(component_editing()&&gizmo_==GizmoMode::region_vertices&&(previous_vertex_.clicked()||next_vertex_.clicked()))
            if(const auto* r=find_region(world,static_cast<u32>(tool_.selected()));r&&!r->points.empty()) {
                const auto count=static_cast<u32>(r->points.size());
                const auto index=tool_.point()<count
                    ? (tool_.point()+count+(next_vertex_.clicked()?1:-1))%count
                    : next_vertex_.clicked()?0:count-1;
                tool_.select(r->id,index);
            }
        if(apply_.clicked())if(const auto* r=find_region(world,static_cast<u32>(tool_.selected()))) {
            auto next=*r;next.name=name_.getText();next.note=note_.getText();Vec3 p{};bool valid=true;
            for(unsigned a=0;a<3;++a){auto v=number(xyz_[a].getText());if(v)p[a]=*v;else valid=false;}
            if(!valid)message_="Region XYZ needs finite numbers";
            else {
                if(component_editing())place(next,tool_.vertices(),p);
                const auto ids = moved_points(tool_, *r);
                const bool points_only = next.name == r->name && next.note == r->note;
                auto begun=points_only ? editing.begin_region_points(next.id, ids) : editing.begin_region(next.id);
                if(!begun)message_=begun.error().message;
                else {
                    auto changed=points_only ? editing.region_points(point_values(editing.state(), next, ids))
                                             : editing.region(region_to_local(editing.state(),next));
                    if(!changed){(void)editing.cancel();message_=changed.error().message;}
                    else {
                        auto committed=editing.commit();message_=committed?"Region applied":committed.error().message;
                        if (points_only) pending_.regions[next.id].points.insert(ids.begin(), ids.end());
                        else pending_.regions[next.id].whole = true;
                    }
                }
            }
        }
    }
    sync(editing.state());
    const auto selected_before=tool_.selected();
    auto action=tool_.update(camera,viewport,input,raw,editable&&(!editing.busy()||editing.active(EditGesture::region)),arrow_step);
    transform_.value(tool_.transform_mode());
    if(tool_.selected()!=selected_before)selection_=static_cast<u32>(tool_.selected());
    if(auto picked=tool_.picked_object())selection_=static_cast<u32>(*picked);
    if(action.began) {
        const auto* current = find_region(*cached_, static_cast<u32>(action.object));
        auto begun=current ? editing.begin_region_points(current->id, moved_points(tool_, *current))
                           : content::Result<void>{std::unexpected(content::Diagnostic{.message="Unknown region"})};
        if(!begun){tool_.clear();message_=begun.error().message;}
    }
    if(editing.active(EditGesture::region)) {
        const auto id = static_cast<u32>(action.object);
        const auto* current = find_region(*cached_, id);
        const auto ids = current ? moved_points(tool_, *current) : std::vector<u32>{};
        pending_.regions[id].points.insert(ids.begin(), ids.end());
        if(action.cancelled) {auto result=editing.cancel();if(!result)message_=result.error().message;}
        else {
            if(action.changed && current) {
                Region subset;subset.id=id;
                for(const auto& point:action.points)subset.points.push_back(point.position);
                auto local=region_to_local(editing.state(),subset);
                std::vector<RegionPointEdit> values;
                for(std::size_t i=0;i<action.points.size();++i)values.push_back({action.points[i].id,local.points[i]});
                if(auto result=editing.region_points(values);!result){message_=result.error().message;(void)editing.cancel();tool_.clear();}
            }
            if(action.finished&&editing.active(EditGesture::region)) {auto result=editing.commit();if(!result)message_=result.error().message;}
        }
    }
    sync(editing.state());
}
} // namespace editor_example
