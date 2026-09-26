#include "earth_scaffold_gizmos.hpp"
#include <algorithm>
#include <cmath>

namespace editor_example {
namespace {
using namespace vng;
namespace earth=example::earth;
using Part=earth::InfrastructurePart;
MeshDraftEdit edit(std::string label,Part part) {
    return {std::move(label),[part=std::move(part)](const editor::EditableMesh& source)->content::Result<editor::EditableMesh> {
        auto changed=earth::edit_infrastructure(source.document(),part);
        if(!changed)return std::unexpected(changed.error());
        return editor::EditableMesh::create(std::move(*changed));
    }};
}
SurfaceMove marker(const MovePath& path,f32 t,std::string label,Mat4 frame,std::shared_ptr<const MovePath> shared) {
    const auto p=path.sample(t);
    return {{},p,std::hypot(p.x,p.y,p.z),std::move(label),frame,false,{},std::move(shared)};
}
class ScaffoldSetup final : public MeshPartGizmoSetup,public std::enable_shared_from_this<ScaffoldSetup> {
public:
    enum class Kind {add,uniform};
    struct Values {f32 position{.5F},first{.15F},last{.85F},spacing{};};
    ScaffoldSetup(Kind kind,Part part,earth::TunnelArc arc,std::vector<f32> positions,
        Mat4 frame,std::shared_ptr<const MovePath> path)
        :kind_(kind),part_(std::move(part)),arc_(std::move(arc)),positions_(std::move(positions)),frame_(frame),path_(std::move(path)) {
        values_.spacing=(arc_.distance(values_.last)-arc_.distance(values_.first))/4;
    }
    std::vector<MeshPartHandle> handles() const override {
        const auto handle=[&](f32 t,std::string label){return MeshPartHandle{marker(*path_,t,std::move(label),frame_,path_),{},{}};};
        if(kind_==Kind::add)return {handle(values_.position,"New support")};
        return {handle(values_.first,"First interior support"),handle(values_.last,"Last interior support"),
            handle(arc_.parameter(arc_.distance(values_.first)+values_.spacing),"Spacing")};
    }
    void begin() override {original_=values_;}
    void move(std::size_t index,Vec3 p) override {
        const auto t=path_->parameter(p);
        if(kind_==Kind::add)values_.position=t;
        else if(index==0)values_.first=std::clamp(t,.001F,values_.last-.001F);
        else if(index==1)values_.last=std::clamp(t,values_.first+.001F,.999F);
        else values_.spacing=std::max(arc_.length()*.0001F,arc_.distance(t)-arc_.distance(values_.first));
    }
    void finish(bool cancel) override {if(cancel)values_=original_;}
    void describe(editor::Inspector& ui,SubmitMeshDraftEdit submit) override {
        const auto self=shared_from_this();
        auto settings=ui.edit("scaffold_setup",values_,kind_==Kind::add?"Position along tunnel":"Uniform support setup");
        if(kind_==Kind::add)settings.slider("position",&Values::position,0,1,"Position along tunnel");
        else {
            settings.slider("first",&Values::first,.001F,.998F,"First interior position");
            settings.slider("last",&Values::last,.002F,.999F,"Last interior position");
            settings.field("spacing",&Values::spacing,"Spacing (Earth radii)");
        }
        settings.live([self](const Values& value)->editor::Result<void> {
            if(!std::isfinite(value.position)||!std::isfinite(value.first)||!std::isfinite(value.last)||!std::isfinite(value.spacing)||
                value.position<0||value.position>1||value.first<=0||value.first>=value.last||value.last>=1||value.spacing<=0)
                return std::unexpected(editor::Diagnostic{"Use ordered interior positions and positive spacing"});
            self->values_=value;return {};
        });
        ui.action("apply_scaffold_setup",[self,submit]()->editor::Result<void> {
            auto positions=self->positions_;
            if(self->kind_==Kind::uniform) {
                const auto& v=self->values_;
                auto result=earth::uniform_tunnel_supports(self->arc_,v.first,v.last,v.spacing,positions);
                if(!result)return std::unexpected(editor::Diagnostic{result.error().message});
                positions=std::move(*result);
            } else {
                if(positions.size()>=64)return std::unexpected(editor::Diagnostic{"Maximum 64 supports"});
                if(std::ranges::any_of(positions,[&](f32 t){return std::abs(t-self->values_.position)<.00001F;}))
                    return std::unexpected(editor::Diagnostic{"A support already occupies this position"});
                positions.push_back(self->values_.position);
            }
            auto part=self->part_;part.scaffold=true;part.scaffold_positions=std::move(positions);
            submit(edit(self->kind_==Kind::add?"Add scaffold":"Uniform scaffold spacing",std::move(part)));return {};
        },kind_==Kind::add?"Add support here":"Apply uniform spacing");
    }
private:
    Kind kind_;
    Part part_;
    earth::TunnelArc arc_;
    std::vector<f32> positions_;
    Mat4 frame_;
    std::shared_ptr<const MovePath> path_;
    Values values_,original_;
};
}
void describe_scaffold_visibility(vng::editor::Inspector& ui,SubmitMeshDraftEdit submit,const example::earth::InfrastructurePart& part) {
    auto values=ui.edit("gizmo_scaffolding",part,"Scaffolding");
    values.toggle("visible",&Part::scaffold,"Show scaffolding");
    values.live([part,submit](const Part& next){auto changed=part;changed.scaffold=next.scaffold;submit(edit("Scaffold visibility",std::move(changed)));});
}
vng::editor::Result<void> append_scaffold_gizmos(BlueprintMeshDescription& description,const Part& p,const earth::TunnelCurve& curve,Mat4 frame) {
    auto positions=earth::tunnel_support_positions(p,curve);
    const bool resolved=bool(positions);
    // An old hidden tunnel can have excessively dense automatic spacing. Keep
    // its reset action reachable rather than losing the entire part inspector.
    if(!resolved)positions=std::vector<f32>{};
    auto path=std::make_shared<MovePath>();
    for(unsigned i=0;i<=128;++i)path->points.push_back(curve.sample(f32(i)/128).position);
    const auto options=[p,resolved](editor::Inspector& ui,SubmitMeshDraftEdit submit){
        if(resolved)describe_scaffold_visibility(ui,submit,p);
        ui.action("reset_scaffold_endpoints",[p,submit]{
            auto next=p;next.scaffold_positions=std::vector<f32>{0,1};
            submit(edit("Reset scaffold endpoints",std::move(next)));
        },"Reset to endpoint supports");
    };
    MeshPartGizmo move{"Scaffold positions",{},options};
    for(std::size_t i=0;i<positions->size();++i) {
        move.handles.push_back({marker(*path,(*positions)[i],"Support "+std::to_string(i+1),frame,path),
            [p,positions=*positions,path,i](Vec3 point) {
                auto next=p;next.scaffold_positions=positions;(*next.scaffold_positions)[i]=path->parameter(point);
                return edit("Scaffold placement",std::move(next));
            },{},[p,positions=*positions,i] {
                auto next=p;next.scaffold_positions=positions;next.scaffold_positions->erase(next.scaffold_positions->begin()+std::ptrdiff_t(i));
                return edit("Delete scaffold",std::move(next));
            }});
    }
    description.gizmos.push_back(std::move(move)); // Even empty: visibility and cycling remain available.
    if(!resolved)return {};
    if(positions->size()<64)description.gizmos.push_back({"Add scaffold",{},options,
        std::make_shared<ScaffoldSetup>(ScaffoldSetup::Kind::add,p,earth::TunnelArc{curve},*positions,frame,path)});
    if(std::ranges::all_of(*positions,[](f32 t){return t==0||t==1;}))
        description.gizmos.push_back({"Uniform scaffold spacing",{},options,
            std::make_shared<ScaffoldSetup>(ScaffoldSetup::Kind::uniform,p,earth::TunnelArc{curve},*positions,frame,path)});
    return {};
}
}
