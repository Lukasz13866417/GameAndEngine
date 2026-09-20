#include "mesh_tools.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <map>

namespace editor_example {
using namespace vng;
namespace {
float area(Vec2 a,Vec2 b,Vec2 c) {return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
}
MeshTools::MeshTools(ui::Container controls,ui::Container popup):popup_(popup),
    modes_(controls.dropdown<MeshSelectMode>("Select",{{MeshSelectMode::vertex,"Vertices (1)"},
        {MeshSelectMode::edge,"Edges (2)"},{MeshSelectMode::face,"Faces (3)"},
        {MeshSelectMode::surface,"Surface (4)"},{MeshSelectMode::whole,"Whole mesh (5)"}})),
    transform_(controls.dropdown<GizmoMode>("Mesh gizmo",{{GizmoMode::move,gizmo_choice_label(GizmoMode::move)},
        {GizmoMode::rotate,gizmo_choice_label(GizmoMode::rotate)},{GizmoMode::scale,gizmo_choice_label(GizmoMode::scale)},
        {GizmoMode::free_rotate,gizmo_choice_label(GizmoMode::free_rotate)}})),
    whole_transform_(controls.dropdown<GizmoMode>("Mesh gizmo",{
        {GizmoMode::rotate,gizmo_choice_label(GizmoMode::rotate)},
        {GizmoMode::scale,gizmo_choice_label(GizmoMode::scale)},
        {GizmoMode::free_rotate,gizmo_choice_label(GizmoMode::free_rotate)}}).visible(false)) {
    summary_=controls.label("1 selected").height(26);
    controls.label("Shift-click: add/remove | A: all").height(24);
    transform_help_=controls.label("G/R/S: transform | Enter: confirm").height(24);
    controls.label("F: edge/face | H: hide | Alt+H: reveal").height(24);
    xray_=controls.checkbox("X-ray selection").value(false).height(26);
    popup_.width(264).height(244).padding(8).gap(6).visible(false);
    popup_.label("Mesh tools").height(24);
    fill_=popup_.button("Make edge / face (F)").height(34);
    subdivide_=popup_.button("Subdivide").height(34);
    align_=popup_.button("Align to line").height(34);
    hide_=popup_.button("Hide faces (H)").height(34);
    reveal_=popup_.button("Reveal hidden (Alt+H)").height(34);
}
void MeshTools::reset() {
    if(!blueprint_ && selected_.empty()) {close();return;}
    blueprint_.reset(); revision_=0; selected_.clear();
    visibility_={};visible_faces_.clear();visible_edges_.clear();visible_vertices_.clear();
    ++visibility_revision_;++selection_revision_;++topology_revision_;close();
}
void MeshTools::sync(const State& state) {
    const auto target=mesh_target(state); const auto* mesh=editable_mesh(state);
    if(!target || !mesh) { reset(); return; }
    if(blueprint_!=target->blueprint || (revision_!=state.document.revision &&
        (count_!=mesh->size() || faces_!=mesh->document().faces || explicit_edges_!=mesh->document().edges))) {
        close(); selected_.clear();
        blueprint_=target->blueprint; count_=mesh->size(); faces_=mesh->document().faces;
        explicit_edges_=mesh->document().edges; edges_=mesh->edges();
        // Face IDs belong to one topology. Do not carry a mask onto new faces
        // after subdividing, undoing topology, or switching blueprints.
        visibility_={*blueprint_,gfx::detail::mesh_topology_fingerprint(count_,faces_),{}};
        update_visibility();
        ++selection_revision_; ++topology_revision_;
        if(mode_==MeshSelectMode::vertex) selected_.push_back(state.viewport.selected_vertex);
    }
    revision_=state.document.revision;
    if(auto next=modes_.changedValue()) mode(*next);
    if(summary_revision_==selection_revision_) return;
    summary_revision_=selection_revision_;
    if(mode_==MeshSelectMode::surface) {
        summary_.text("Surface / click a part to select");
        return;
    }
    if(mode_==MeshSelectMode::whole) {
        summary_.text("Whole mesh / R S / Ctrl+arrows: gizmo");
        return;
    }
    const auto ordered=vertices(*mesh);
    auto summary=std::to_string(selected_.size())+" selected / "+std::to_string(ordered.size())+" vertices";
    if(!visibility_.hidden_faces.empty()) summary+=" / "+std::to_string(visibility_.hidden_faces.size())+" hidden faces";
    if(ordered.size()>=2) summary+=" / line anchors: "+std::to_string(ordered[0])+", "+std::to_string(ordered[1]);
    summary_.text(summary);
}
void MeshTools::mode(MeshSelectMode value) {
    if(mode_!=value) { mode_=value; selected_.clear(); ++selection_revision_; close(); }
    modes_.value(value);
    transform_.enabled(value!=MeshSelectMode::surface);
    transform_.visible(value!=MeshSelectMode::whole);
    whole_transform_.visible(value==MeshSelectMode::whole);
    transform_help_.text(value==MeshSelectMode::whole?"R/S: transform | Ctrl+arrows: gizmo":"G/R/S: transform | Enter: confirm");
    xray_.enabled(component_mode());
}
bool MeshTools::cycle(int direction) {
    if(mode_==MeshSelectMode::surface || !direction)return false;
    constexpr std::array components{GizmoMode::move,GizmoMode::rotate,GizmoMode::scale,GizmoMode::free_rotate};
    const std::span<const GizmoMode> choices=mode_==MeshSelectMode::whole?std::span<const GizmoMode>{whole_mesh_gizmos}:std::span<const GizmoMode>{components};
    const auto index=std::ranges::find(choices,transform_mode())-choices.begin();
    transform_mode(choices[(index+choices.size()+(direction>0?1:-1))%choices.size()]);
    return true;
}
void MeshTools::select(u32 id,bool extend) {
    if(!visible(mode_,id)) return;
    auto found=std::ranges::find(selected_,id);
    if(!extend && selected_.size()==1 && found!=selected_.end()) return;
    ++selection_revision_;
    if(!extend)selected_={id};
    else if(found!=selected_.end())selected_.erase(found);
    else selected_.push_back(id);
}
void MeshTools::select_all(const editor::EditableMesh& mesh,bool clear) {
    ++selection_revision_;
    selected_.clear(); if(clear || !component_mode()) return;
    const auto count=mode_==MeshSelectMode::vertex?mesh.size():mode_==MeshSelectMode::edge?edges_.size():mesh.document().faces.size();
    for(u32 i=0;i<count;++i) if(visible(mode_,i)) selected_.push_back(i);
}
void MeshTools::select(std::span<const u32> ids,editor::SelectionMode mode) {
    editor::Selection<u32> selection;
    if(mode!=editor::SelectionMode::replace) selection.select(selected_);
    selection.select(ids,mode);
    std::vector<u32> next;
    for(auto id:selection.items()) if(visible(mode_,id)) next.push_back(id);
    if(next!=selected_) {selected_=next;++selection_revision_;}
}

std::vector<u32> MeshTools::box(const State& state,ui::Rect rect,Extent2D extent,const gfx::Camera& camera) const {
    if(!component_mode()) return {};
    const auto points=project_vertices(state,extent,&camera,false);
    // Bin projected faces once. Testing every candidate against every face was
    // quadratic on dense ships; each point now tests only its screen tile.
    constexpr int side=32;
    std::array<std::vector<u32>,side*side> bins;
    const auto cell=[](f32 x){return std::clamp(static_cast<int>(std::floor(std::clamp(x,0.F,1.F)*side)),0,side-1);};
    if(!xray()) for(u32 i=0;i<faces_.size();++i) {
        if(!visible(MeshSelectMode::face,i)) continue;
        const auto f=faces_[i];if(!points[f[0]]||!points[f[1]]||!points[f[2]]) continue;
        const auto a=*points[f[0]],b=*points[f[1]],c=*points[f[2]];
        const auto lowx=std::min({a.x,b.x,c.x}),highx=std::max({a.x,b.x,c.x});
        const auto lowy=std::min({a.y,b.y,c.y}),highy=std::max({a.y,b.y,c.y});
        if(highx<rect.x || lowx>rect.x+rect.width || highy<rect.y || lowy>rect.y+rect.height) continue;
        for(int y=cell(lowy);y<=cell(highy);++y) for(int x=cell(lowx);x<=cell(highx);++x) bins[y*side+x].push_back(i);
    }
    const auto visible=[&](Vec3 q) {
        if(xray()) return true;
        for(auto index:bins[cell(q.y)*side+cell(q.x)]) {
            const auto f=faces_[index];const auto a=*points[f[0]],b=*points[f[1]],c=*points[f[2]];
            if(q.z<=std::min({a.z,b.z,c.z})+.00004F) continue;
            const auto total=area({a.x,a.y},{b.x,b.y},{c.x,c.y});if(std::abs(total)<1e-12F) continue;
            const Vec2 p{q.x,q.y};
            const auto u=area(p,{b.x,b.y},{c.x,c.y})/total,v=area({a.x,a.y},p,{c.x,c.y})/total,w=1-u-v;
            if(u>=-1e-5F&&v>=-1e-5F&&w>=-1e-5F&&u*a.z+v*b.z+w*c.z<q.z-.00004F) return false;
        }
        return true;
    };
    const auto inside=[&](Vec3 p){return p.x>=rect.x && p.x<=rect.x+rect.width && p.y>=rect.y && p.y<=rect.y+rect.height;};
    std::vector<u32> selected;
    if(mode_==MeshSelectMode::vertex) {
        for(u32 i=0;i<points.size();++i) if(this->visible(mode_,i) && points[i] && inside(*points[i]) && visible(*points[i])) selected.push_back(i);
    } else {
        const auto count=mode_==MeshSelectMode::edge?edges_.size():faces_.size();
        for(u32 i=0;i<count;++i) {
            if(!this->visible(mode_,i)) continue;
            Vec3 center{};bool contained=true;
            const auto n=mode_==MeshSelectMode::edge?2U:3U;
            for(unsigned v=0;v<n;++v) {
                const auto id=mode_==MeshSelectMode::edge?edges_[i][v]:faces_[i][v];
                if(!points[id] || !inside(*points[id])) {contained=false;break;}
                for(unsigned c=0;c<3;++c) center[c]+=(*points[id])[c]/static_cast<f32>(n);
            }
            if(contained && visible(center)) selected.push_back(i);
        }
    }
    return selected;
}
std::vector<u32> MeshTools::vertices(const editor::EditableMesh& mesh,bool weld) const {
    std::vector<u32> result; std::set<u32> seen;
    const auto add=[&](u32 id){if(id<mesh.size() && visible(MeshSelectMode::vertex,id) && seen.insert(id).second) result.push_back(id);};
    for(auto id:selected_) {
        if(mode_==MeshSelectMode::vertex) add(id);
        else if(mode_==MeshSelectMode::edge && id<edges_.size()) { add(edges_[id][0]);add(edges_[id][1]); }
        else if(mode_==MeshSelectMode::face && id<mesh.document().faces.size())
            for(auto v:mesh.document().faces[id].vertices) add(v);
    }
    if(weld && !result.empty() && result.size()<mesh.size()) {
        // Look up neighbouring spatial cells once per candidate, rather than
        // scanning the entire mesh for every selected vertex (quadratic for A).
        constexpr double tolerance=.00001;
        using Cell=std::array<std::int64_t,3>;
        const auto cell=[](Vec3 p) {return Cell{static_cast<std::int64_t>(std::floor(double(p.x)/tolerance)),
            static_cast<std::int64_t>(std::floor(double(p.y)/tolerance)),static_cast<std::int64_t>(std::floor(double(p.z)/tolerance))};};
        std::map<Cell,std::vector<Vec3>> cells;
        for(auto id:result) {const auto p=mesh.position(id);cells[cell(p)].push_back(p);}
        for(u32 id=0;id<mesh.size();++id) if(!seen.contains(id)) {
            const auto p=mesh.position(id);const auto at=cell(p);bool found{};
            for(int x=-1;x<=1 && !found;++x) for(int y=-1;y<=1 && !found;++y) for(int z=-1;z<=1 && !found;++z) {
                const auto bucket=cells.find({at[0]+x,at[1]+y,at[2]+z});if(bucket==cells.end()) continue;
                found=std::ranges::any_of(bucket->second,[&](Vec3 q) {return std::abs(double(p.x)-q.x)<=tolerance &&
                    std::abs(double(p.y)-q.y)<=tolerance && std::abs(double(p.z)-q.z)<=tolerance;});
            }
            if(found) add(id);
        }
    }
    return result;
}
std::vector<gfx::Edge> MeshTools::edges(const editor::EditableMesh& mesh) const {
    std::vector<gfx::Edge> result;
    if(mode_==MeshSelectMode::edge) { for(auto id:selected_) if(id<edges_.size()) result.push_back(edges_[id]); return result; }
    std::set<std::pair<u32,u32>> pairs;
    if(mode_==MeshSelectMode::face) {
        for(auto id:selected_) if(id<mesh.document().faces.size()) {
            const auto& f=mesh.document().faces[id];
            for(unsigned i=0;i<3;++i) pairs.emplace(std::min(f[i],f[(i+1)%3]),std::max(f[i],f[(i+1)%3]));
        }
    } else {
        const std::set<u32> chosen(selected_.begin(),selected_.end());
        for(u32 id=0;id<edges_.size();++id) if(visible(MeshSelectMode::edge,id)) {
            const auto e=edges_[id];
            if(chosen.contains(e[0]) && chosen.contains(e[1])) pairs.emplace(e[0],e[1]);
        }
    }
    for(auto [a,b]:pairs) result.emplace_back(a,b);
    return result;
}
std::optional<u32> MeshTools::pick(const State& state,Vec2 p,Extent2D extent,const gfx::Camera& camera,ui::Rect bounds) const {
    if(!component_mode()) return {};
    // Offscreen corners still belong to triangles covering the viewport.
    const auto points=project_vertices(state,extent,&camera,false);
    float nearest=100,depth=2; std::optional<u32> found;
    // Selection uses the same visible-surface rule as the GPU overlay. This
    // runs only on a click, never on every frame; X-ray deliberately bypasses it.
    const auto visible=[&](Vec3 q) {
        if(xray()) return true;
        for(u32 index=0;index<faces_.size();++index) {
            if(!this->visible(MeshSelectMode::face,index)) continue;
            const auto& f=faces_[index];
            if(!points[f[0]]||!points[f[1]]||!points[f[2]]) continue;
            const auto a=*points[f[0]],b=*points[f[1]],c=*points[f[2]];
            if(q.z<=std::min({a.z,b.z,c.z})+.00004F) continue;
            const Vec2 xy{q.x,q.y};
            const auto total=area({a.x,a.y},{b.x,b.y},{c.x,c.y});if(std::abs(total)<1e-12F) continue;
            const auto u=area(xy,{b.x,b.y},{c.x,c.y})/total,v=area({a.x,a.y},xy,{c.x,c.y})/total,w=1-u-v;
            if(u>=-1e-5F&&v>=-1e-5F&&w>=-1e-5F && u*a.z+v*b.z+w*c.z<q.z-.00004F) return false;
        }
        return true;
    };
    if(mode_==MeshSelectMode::vertex) {
        for(u32 i=0;i<points.size();++i) if(this->visible(mode_,i) && points[i]) {
            const auto q=*points[i]; const auto dx=(q.x-p.x)*bounds.width,dy=(q.y-p.y)*bounds.height,d=dx*dx+dy*dy;
            if((d<nearest-.01F || (std::abs(d-nearest)<.01F && q.z<depth)) && visible(q)) {nearest=d;depth=q.z;found=i;}
        }
    } else if(mode_==MeshSelectMode::edge) {
        for(u32 i=0;i<edges_.size();++i) {
            if(!this->visible(mode_,i)) continue;
            auto e=edges_[i]; if(!points[e[0]]||!points[e[1]]) continue;
            auto a=*points[e[0]],b=*points[e[1]];
            Vec2 ab{(b.x-a.x)*bounds.width,(b.y-a.y)*bounds.height}, ap{(p.x-a.x)*bounds.width,(p.y-a.y)*bounds.height};
            const auto length=ab.x*ab.x+ab.y*ab.y; if(length<.0001F) continue;
            const auto t=std::clamp((ap.x*ab.x+ap.y*ab.y)/length,0.F,1.F);
            const auto dx=ap.x-t*ab.x,dy=ap.y-t*ab.y,d=dx*dx+dy*dy,z=a.z+t*(b.z-a.z);
            if((d<nearest-.01F || (std::abs(d-nearest)<.01F && z<depth)) && visible({a.x+t*(b.x-a.x),a.y+t*(b.y-a.y),z})) {nearest=d;depth=z;found=i;}
        }
    } else {
        const auto* mesh=editable_mesh(state); if(!mesh) return {};
        for(u32 i=0;i<mesh->document().faces.size();++i) {
            if(!this->visible(mode_,i)) continue;
            const auto f=mesh->document().faces[i];
            if(!points[f[0]]||!points[f[1]]||!points[f[2]]) continue;
            const auto a=*points[f[0]],b=*points[f[1]],c=*points[f[2]];
            const auto total=area({a.x,a.y},{b.x,b.y},{c.x,c.y}); if(std::abs(total)<1e-12F) continue;
            const auto u=area(p,{b.x,b.y},{c.x,c.y})/total,v=area({a.x,a.y},p,{c.x,c.y})/total,w=1-u-v;
            const auto z=u*a.z+v*b.z+w*c.z;
            if(u>=-1e-5F&&v>=-1e-5F&&w>=-1e-5F&&z<depth) {depth=z;found=i;}
        }
    }
    return found;
}
void MeshTools::open(Vec2 at,Vec2 screen,std::optional<ui::Rect> viewport) {
    if(viewport)at={viewport->x+8,viewport->y+std::max(0.F,viewport->height-252)};
    popup_.position({std::clamp(at.x,0.F,std::max(0.F,screen.x-264)),std::clamp(at.y,0.F,std::max(0.F,screen.y-244))}).visible(true);
    fill_.enabled(!selected_.empty() && (selected_.size()>=2 || mode_!=MeshSelectMode::vertex));
    subdivide_.enabled(!selected_.empty()); align_.enabled(!selected_.empty()); menu_open_=true;
    hide_.enabled(!selected_.empty());reveal_.enabled(!visibility_.hidden_faces.empty());
}
void MeshTools::close() { menu_open_=false; popup_.visible(false); }
std::optional<MeshAction> MeshTools::poll(std::span<const input::Event> events) {
    if(!menu_open_) return {};
    std::optional<MeshAction> action;
    if(fill_.clicked()) action=MeshAction::fill;
    if(subdivide_.clicked()) action=MeshAction::subdivide;
    if(align_.clicked()) action=MeshAction::align;
    if(hide_.clicked()) action=MeshAction::hide;
    if(reveal_.clicked()) action=MeshAction::reveal;
    if(action) { close(); return action; }
    for(const auto& e:events) if(e.kind==input::EventKind::focus_lost ||
        (e.kind==input::EventKind::key_down && e.key==input::Key::escape) ||
        (e.kind==input::EventKind::pointer_down && !popup_.bounds().contains(e.position))) close();
    return {};
}
bool MeshTools::visible(MeshSelectMode mode,u32 id) const {
    if(mode==MeshSelectMode::surface || mode==MeshSelectMode::whole) return false;
    const auto& flags=mode==MeshSelectMode::face?visible_faces_:mode==MeshSelectMode::edge?visible_edges_:visible_vertices_;
    return id<flags.size() && flags[id];
}
void MeshTools::update_visibility() {
    visible_faces_.assign(faces_.size(),true);
    if(visibility_.hidden_faces.empty()) {
        visible_vertices_.assign(count_,true);visible_edges_.assign(edges_.size(),true);
        ++visibility_revision_;++selection_revision_;return;
    }
    for(auto id:visibility_.hidden_faces) visible_faces_[id]=false;
    visible_vertices_.assign(count_,false);
    std::vector<bool> used(count_);
    std::map<std::pair<u32,u32>,bool> edges;
    for(u32 id=0;id<faces_.size();++id) {
        const auto f=faces_[id];
        for(unsigned c=0;c<3;++c) {
            used[f[c]]=true;
            if(visible_faces_[id]) visible_vertices_[f[c]]=true;
            auto& visible=edges[std::minmax(f[c],f[(c+1)%3])];
            visible=visible || visible_faces_[id];
        }
    }
    // Loose geometry remains visible: H hides faces, not standalone points/edges.
    for(u32 id=0;id<count_;++id) if(!used[id]) visible_vertices_[id]=true;
    visible_edges_.clear();visible_edges_.reserve(edges_.size());
    for(auto e:edges_) {
        const auto found=edges.find(std::minmax(e[0],e[1]));
        const bool show=found==edges.end() || found->second;
        visible_edges_.push_back(show);
        if(show) visible_vertices_[e[0]]=visible_vertices_[e[1]]=true;
    }
    ++visibility_revision_;++selection_revision_;
}
std::size_t MeshTools::hide_selected() {
    if(selected_.empty()) return 0;
    std::vector<bool> chosen(mode_==MeshSelectMode::vertex?count_:mode_==MeshSelectMode::edge?edges_.size():faces_.size());
    for(auto id:selected_) if(id<chosen.size()) chosen[id]=true;
    std::set<std::pair<u32,u32>> chosen_edges;
    if(mode_==MeshSelectMode::edge) for(auto id:selected_) if(id<edges_.size()) chosen_edges.insert(std::minmax(edges_[id][0],edges_[id][1]));
    const auto before=visibility_.hidden_faces.size();
    for(u32 id=0;id<faces_.size();++id) {
        if(!visible_faces_[id]) continue;
        const auto f=faces_[id];
        bool hide=mode_==MeshSelectMode::face && chosen[id];
        for(unsigned c=0;c<3 && !hide;++c) {
            if(mode_==MeshSelectMode::vertex) hide=chosen[f[c]];
            if(mode_==MeshSelectMode::edge) hide=chosen_edges.contains(std::minmax(f[c],f[(c+1)%3]));
        }
        if(hide) visibility_.hidden_faces.push_back(id);
    }
    const auto count=visibility_.hidden_faces.size()-before;
    if(count) {
        std::ranges::sort(visibility_.hidden_faces);
        selected_.clear();update_visibility();close();
    }
    return count;
}
std::size_t MeshTools::reveal_hidden() {
    const auto count=visibility_.hidden_faces.size();
    if(count) {visibility_.hidden_faces.clear();update_visibility();close();}
    return count;
}
} // namespace editor_example
