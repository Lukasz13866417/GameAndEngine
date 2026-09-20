#include "cage_tool.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

namespace editor_example {
namespace {
using namespace vng;
Vec4 clip(Vec3 p,const gfx::CameraSnapshot& camera) {
    Vec4 result{};for(unsigned r=0;r<4;++r) {
        double v=camera.view_projection[3][r];for(unsigned c=0;c<3;++c)v+=double(camera.view_projection[c][r])*p[c];
        result[r]=static_cast<float>(v);
    }return result;
}
Vec2 pixel(Vec4 p,ui::Rect r){return {r.x+(p.x/p.w+1)*.5F*r.width,r.y+(1-p.y/p.w)*.5F*r.height};}
bool inside(Vec4 p){return std::isfinite(p.w)&&p.w>0&&std::abs(p.x)<=p.w&&std::abs(p.y)<=p.w&&std::abs(p.z)<=p.w;}
bool segment(Vec4& a,Vec4& b) {
    for(unsigned plane=0;plane<6;++plane) {
        auto axis=plane/2;auto sign=plane%2?1.F:-1.F;auto da=a.w+sign*a[axis],db=b.w+sign*b[axis];
        if(da<0&&db<0)return false;
        if((da<0)!=(db<0)) {
            const auto t=da/(da-db);Vec4 p{};for(unsigned i=0;i<4;++i)p[i]=a[i]+t*(b[i]-a[i]);(da<0?a:b)=p;
        }
    }return a.w>0&&b.w>0;
}
float distance(Vec2 p,Vec2 a,Vec2 b) {
    const auto x=b.x-a.x,y=b.y-a.y,s=x*x+y*y;
    const auto t=s>0?std::clamp(((p.x-a.x)*x+(p.y-a.y)*y)/s,0.F,1.F):0;
    return std::hypot(p.x-a.x-t*x,p.y-a.y-t*y);
}
void line(ui::DrawList& list,Vec2 a,Vec2 b,ui::Rect r,Vec4 color) {
    const auto d=std::hypot(b.x-a.x,b.y-a.y);if(d<.01F||!std::isfinite(d))return;
    const Vec2 n{(a.y-b.y)/d,(b.x-a.x)/d};
    const Vec2 p{a.x+n.x,a.y+n.y},q{a.x-n.x,a.y-n.y},s{b.x+n.x,b.y+n.y},t{b.x-n.x,b.y-n.y};
    list.commands.emplace_back(ui::TriangleDraw{{p,q,s},r,color});list.commands.emplace_back(ui::TriangleDraw{{q,t,s},r,color});
}
}
void CageTool::select(u64 object,u32 point) {
    if(dragging())return;
    object_=object;point_=point;mode_=editor::CageElement::vertex;elements_={point};++selection_revision_;
    const auto cage=std::ranges::find(cages_,object_,&editor::SceneCage::object);
    if(cage!=cages_.end()&&point==cage->primary_point)elements_.clear();
}
void CageTool::select_elements(editor::CageElement mode,std::span<const u32> elements) {
    if(dragging())return;
    mode_=mode;elements_.assign(elements.begin(),elements.end());++selection_revision_;
    const auto cage=std::ranges::find(cages_,object_,&editor::SceneCage::object);
    if(cage!=cages_.end())point_=mode==editor::CageElement::vertex&&elements_.size()==1?elements_[0]:cage->primary_point;
    transform_.cancel();
}
void CageTool::refresh(std::span<const editor::SceneCage> cages) {
    cages_.assign(cages.begin(),cages.end());
    validate_selection();
}
void CageTool::refresh(editor::SceneCage cage) {
    const auto found = std::ranges::find(cages_, cage.object, &editor::SceneCage::object);
    if (found == cages_.end()) cages_.push_back(std::move(cage));
    else *found = std::move(cage);
    validate_selection();
}
void CageTool::refresh_points(u64 object, std::span<const editor::ScenePoint> points) {
    const auto cage = std::ranges::find(cages_, object, &editor::SceneCage::object);
    if (cage == cages_.end()) return;
    for (const auto& point : points) {
        const auto found = std::ranges::find(cage->points, point.id, &editor::ScenePoint::id);
        if (found != cage->points.end()) found->position = point.position;
    }
}
void CageTool::erase(u64 object) {
    std::erase_if(cages_, [&](const auto& cage) { return cage.object == object; });
    if (object_ == object) { transform_.cancel(); object_ = 0; elements_.clear(); ++selection_revision_; }
}
void CageTool::validate_selection() {
    const auto cage=std::ranges::find(cages_,object_,&editor::SceneCage::object);
    if(cage==cages_.end())return;
    const auto before=elements_.size();
    std::erase_if(elements_,[&](auto id){
        if(mode_==editor::CageElement::vertex)
            return id==cage->primary_point||std::ranges::find(cage->points,id,&editor::ScenePoint::id)==cage->points.end();
        return id>=(mode_==editor::CageElement::edge?cage->edges.size():cage->faces.size());
    });
    if(before!=elements_.size())++selection_revision_;
    point_=mode_==editor::CageElement::vertex&&elements_.size()==1?elements_[0]:cage->primary_point;
}
std::vector<u32> CageTool::vertices() const {
    std::vector<u32> result;
    const auto cage=std::ranges::find(cages_,object_,&editor::SceneCage::object);
    if(cage==cages_.end())return result;
    const auto add=[&](u32 id){if(std::ranges::find(result,id)==result.end())result.push_back(id);};
    const auto index=[&](u32 id){if(id<cage->points.size())add(cage->points[id].id);};
    for(auto id:elements_) {
        if(mode_==editor::CageElement::vertex)add(id);
        else if(mode_==editor::CageElement::edge&&id<cage->edges.size())for(auto v:cage->edges[id])index(v);
        else if(mode_==editor::CageElement::face&&id<cage->faces.size())for(auto v:cage->faces[id])index(v);
    }
    return result;
}
std::optional<Vec3> CageTool::pivot() const {
    const auto cage=std::ranges::find(cages_,object_,&editor::SceneCage::object);
    if(cage==cages_.end())return {};
    auto ids=vertices();if(ids.empty())ids.push_back(cage->primary_point);
    Vec3 center{};unsigned count{};
    for(auto id:ids)if(auto p=std::ranges::find(cage->points,id,&editor::ScenePoint::id);p!=cage->points.end()) {
        ++count;for(unsigned c=0;c<3;++c)center[c]+=p->position[c];
    }
    if(!count)return {};
    for(unsigned c=0;c<3;++c)center[c]/=static_cast<float>(count);
    return center;
}
void CageTool::select_all() {
    const auto cage=std::ranges::find(cages_,object_,&editor::SceneCage::object);if(cage==cages_.end())return;
    std::vector<u32> ids;
    if(mode_==editor::CageElement::vertex) {for(const auto& p:cage->points)if(p.id!=cage->primary_point)ids.push_back(p.id);}
    else {ids.resize(mode_==editor::CageElement::edge?cage->edges.size():cage->faces.size());std::iota(ids.begin(),ids.end(),0U);}
    select_elements(mode_,ids);
}
CageAction CageTool::update(const gfx::CameraSnapshot& camera,
    ui::Rect viewport,std::span<const input::Event> input,std::span<const input::Event> raw,bool enabled,float arrow_step) {
    const bool was=dragging();
    CageAction action{object_,point_};handled_=false;hidden_=false;picked_.reset();camera_=camera;viewport_=viewport;
    if(std::ranges::find(cages_,object_,&editor::SceneCage::object)==cages_.end())object_=0;
    const auto selected_points=[&] {
        std::vector<editor::ScenePoint> points;
        const auto cage=std::ranges::find(cages_,object_,&editor::SceneCage::object);
        if(cage!=cages_.end()) {
            auto ids=vertices();
            for(const auto& p:cage->points)if(p.id!=cage->primary_point&&
                std::ranges::find(ids,p.id)!=ids.end())points.push_back(p);
        }
        return points;
    };
    const auto box_event=[&](const input::Event& event) {
        if(auto selected=box_.update(event)) {
            std::vector<u32> hits;
            const auto cage=std::ranges::find(cages_,object_,&editor::SceneCage::object);
            if(cage!=cages_.end()) {
                const auto count=mode_==editor::CageElement::vertex?cage->points.size():mode_==editor::CageElement::edge?cage->edges.size():cage->faces.size();
                for(u32 i=0;i<count;++i) {
                    auto id=mode_==editor::CageElement::vertex?cage->points[i].id:i;
                    if(id==cage->primary_point)continue;
                    if(auto p=element_handle(mode_,id);p&&selected->rect.contains(*p))hits.push_back(id);
                }
                auto next=elements_;
                if(selected->mode==editor::SelectionMode::replace)next.clear();
                for(auto id:hits) {
                    auto found=std::ranges::find(next,id);
                    if(selected->mode==editor::SelectionMode::remove){if(found!=next.end())next.erase(found);}
                    else if(found==next.end())next.push_back(id);
                }
                select_elements(mode_,next);
            }
        }
    };
    if(box_.active()) {
        handled_=true;
        if(!enabled)box_.cancel();else for(const auto& event:raw)box_event(event);
        return action;
    }
    auto result=transform_.update(selected_points(),{object_,selection_revision_,1},camera,viewport,input,raw,
        enabled&&components_&&pivot().has_value(),{},arrow_step);
    handled_=transform_.handled();
    action.began=result.began;action.changed=result.changed;action.finished=result.finished;action.cancelled=result.cancelled;
    action.points=std::move(result.points);
    if(!action.points.empty())for(const auto& p:action.points)for(unsigned c=0;c<3;++c)action.position[c]+=p.position[c]/static_cast<float>(action.points.size());
    if(!was&&!dragging()&&!handled_&&enabled)for(const auto& event:input) {
        if(box_.active()){box_event(event);continue;}
        if(components_&&event.kind==input::EventKind::key_down&&event.key==input::Key::a&&!event.modifiers.control) {
            if(event.modifiers.shift)select_elements(mode_,{});else select_all();handled_=true;continue;
        }
        if(event.kind!=input::EventKind::pointer_down||event.button!=0||!viewport.contains(event.position))continue;
        float best=9;vng::u64 hit{};vng::u32 handle{};bool component=false;
        for(const auto& candidate:cages_) {
            if(candidate.object==object_&&mode_!=editor::CageElement::vertex)continue;
            for(const auto& p:candidate.points) {
                auto c=clip(p.position,camera);if(!inside(c))continue;
                auto s=pixel(c,viewport);auto d=std::hypot(s.x-event.position.x,s.y-event.position.y);
                if(!components_&&candidate.object==object_)continue;
                if(d<best){best=d;hit=candidate.object;handle=components_?p.id:candidate.primary_point;component=components_&&p.id!=candidate.primary_point;}
            }
        }
        if(!hit)for(const auto& candidate:cages_)for(u32 index=0;index<candidate.edges.size();++index) {
            if(candidate.object==object_&&mode_==editor::CageElement::face)continue;
            auto edge=candidate.edges[index];
            if(edge[0]>=candidate.points.size()||edge[1]>=candidate.points.size())continue;
            auto a=clip(candidate.points[edge[0]].position,camera),b=clip(candidate.points[edge[1]].position,camera);
            if(!segment(a,b))continue;
            auto d=distance(event.position,pixel(a,viewport),pixel(b,viewport));
            if(d<6&&d<best&&std::ranges::find(candidate.points,candidate.primary_point,&editor::ScenePoint::id)!=candidate.points.end())
                {best=d;hit=candidate.object;component=components_&&candidate.object==object_&&mode_==editor::CageElement::edge;handle=component?index:candidate.primary_point;}
        }
        if(!hit&&components_&&mode_==editor::CageElement::face) {
            const auto candidate=std::ranges::find(cages_,object_,&editor::SceneCage::object);
            float depth=2;
            if(candidate!=cages_.end())for(u32 index=0;index<candidate->faces.size();++index) {
                std::vector<Vec2> polygon;float z{};bool valid=true;
                for(auto v:candidate->faces[index]) {
                    if(v>=candidate->points.size()){valid=false;break;}
                    auto c=clip(candidate->points[v].position,camera);
                    if(c.w<=0){valid=false;break;}
                    polygon.push_back(pixel(c,viewport));z+=c.z/c.w;
                }
                if(!valid||polygon.size()<3)continue;
                bool contains=false;auto p=event.position;
                for(std::size_t i=0,j=polygon.size()-1;i<polygon.size();j=i++) {
                    auto a=polygon[i],b=polygon[j];
                    if((a.y>p.y)!=(b.y>p.y)&&p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x)contains=!contains;
                }
                z/=static_cast<float>(polygon.size());
                if(contains&&z<depth&&z>=-1&&z<=1){hit=candidate->object;handle=index;component=true;depth=z;}
            }
        }
        if(!hit&&components_&&object_) {
            box_.begin(event.position,viewport,event.modifiers);
            if(!event.modifiers.shift&&!event.modifiers.control)select_elements(mode_,{});
            handled_=true;continue;
        }
        const bool same=hit==object_;if(hit)object_=hit;handled_=hit!=0;
        if(hit&&!components_)picked_=hit;
        if(!same){mode_=editor::CageElement::vertex;elements_.clear();}
        if(hit) {
            if(component) {
                const bool extend=same&&(event.modifiers.shift||event.modifiers.control);
                auto found=std::ranges::find(elements_,handle);
                if(extend&&found!=elements_.end())elements_.erase(found);
                else if(found==elements_.end()||!extend){if(!extend)elements_.clear();elements_.push_back(handle);}
            } else elements_.clear();
            ++selection_revision_;validate_selection();
        }
        if(!hit)transform_.cancel();
        if(hit) { // Show the newly selected handle in this very input frame.
            (void)transform_.update(selected_points(),{object_,selection_revision_,1},camera,viewport,{}, {},enabled&&components_);
        }
    }
    return action;
}
std::optional<Vec2> CageTool::point_handle(u64 object,u32 id) const {
    const auto cage=std::ranges::find(cages_,object,&editor::SceneCage::object);if(cage==cages_.end())return {};
    const auto p=std::ranges::find(cage->points,id,&editor::ScenePoint::id);if(p==cage->points.end())return {};
    auto c=clip(p->position,camera_);if(!inside(c))return {};return pixel(c,viewport_);
}
std::optional<Vec2> CageTool::element_handle(editor::CageElement kind,u32 id) const {
    if(kind==editor::CageElement::vertex)return point_handle(object_,id);
    const auto cage=std::ranges::find(cages_,object_,&editor::SceneCage::object);if(cage==cages_.end())return {};
    std::vector<u32> indices;
    if(kind==editor::CageElement::edge&&id<cage->edges.size())indices.assign(cage->edges[id].begin(),cage->edges[id].end());
    if(kind==editor::CageElement::face&&id<cage->faces.size())indices=cage->faces[id];
    if(indices.empty())return {};
    Vec3 center{};
    for(auto v:indices) {
        if(v>=cage->points.size())return {};
        for(unsigned a=0;a<3;++a)center[a]+=cage->points[v].position[a]/static_cast<float>(indices.size());
    }
    const auto c=clip(center,camera_);if(!inside(c))return {};
    return pixel(c,viewport_);
}
void CageTool::append(ui::DrawList& list,const text::Font& font,bool boundaries) const {
    if (hidden_) return;
    const auto selected_vertices=vertices();
    for(const auto& cage:cages_) {
        std::set<std::array<u32,2>> highlighted;
        if(cage.object==object_&&mode_==editor::CageElement::face)for(auto face:elements_)if(face<cage.faces.size()) {
            const auto& f=cage.faces[face];
            for(std::size_t i=0;i<f.size();++i) {
                auto a=f[i],b=f[(i+1)%f.size()];if(a>b)std::swap(a,b);highlighted.insert({a,b});
            }
        }
        const Vec4 color=cage.object==object_?Vec4{1,.65F,.15F,1}:Vec4{.2F,.85F,.8F,.7F};
        for(u32 index=0;index<cage.edges.size();++index) {
            auto edge=cage.edges[index];
            if(edge[0]>=cage.points.size()||edge[1]>=cage.points.size())continue;
            auto a=clip(cage.points[edge[0]].position,camera_),b=clip(cage.points[edge[1]].position,camera_);
            auto selected=cage.object==object_&&mode_==editor::CageElement::edge&&std::ranges::find(elements_,index)!=elements_.end();
            auto key=edge;if(key[0]>key[1])std::swap(key[0],key[1]);selected|=highlighted.contains(key);
            if(!boundaries&&!selected)continue; // Only interaction highlights are x-ray overlays.
            if(segment(a,b))line(list,pixel(a,viewport_),pixel(b,viewport_),viewport_,selected?Vec4{1,1,1,1}:color);
        }
        for(const auto& p:cage.points) {
            if(!components_&&p.id!=cage.primary_point)continue;
            auto c=clip(p.position,camera_);if(!inside(c))continue;
            const bool selected=cage.object==object_&&std::ranges::find(selected_vertices,p.id)!=selected_vertices.end();
            auto s=pixel(c,viewport_);const float r=selected?5:3;
            line(list,{s.x-r,s.y},{s.x+r,s.y},viewport_,selected?Vec4{1,1,1,1}:color);
            line(list,{s.x,s.y-r},{s.x,s.y+r},viewport_,selected?Vec4{1,1,1,1}:color);
        }
        if(cage.object==object_&&mode_==editor::CageElement::face)for(u32 i=0;i<cage.faces.size();++i)
            if(auto p=element_handle(mode_,i)) {
                line(list,{p->x-3,p->y},{p->x+3,p->y},viewport_,color);
                line(list,{p->x,p->y-3},{p->x,p->y+3},viewport_,color);
            }
        const auto label_point=std::ranges::find(cage.points,cage.primary_point,&editor::ScenePoint::id);
        if(font.valid()&&label_point!=cage.points.end()) {
            auto c=clip(label_point->position,camera_);if(inside(c)) {
                auto s=pixel(c,viewport_);s.x+=10;s.y+=10;
                list.commands.emplace_back(ui::TextDraw{cage.label,s,viewport_,font,14,color});
            }
        }
    }transform_.append(list,font);box_.append(list);
}
} // namespace editor_example
