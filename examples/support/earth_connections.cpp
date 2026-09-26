#include "earth_connections.hpp"
#include "earth_structures.hpp"
#include "earth_infrastructure_placement.hpp"
#include <set>

namespace example::earth {
namespace {
using namespace vng;
using namespace placement;
auto error(std::string message){return std::unexpected(mesh_frame::error(std::move(message)));}
f32 length(Vec3 v){return std::hypot(v.x,v.y,v.z);}
f32 smooth(f32 t){t=std::clamp(t,0.F,1.F);return t*t*(3-2*t);}
}
std::vector<TunnelSocket> infrastructure_sockets(std::span<const InfrastructurePart> parts,InfrastructureSettings settings) {
    std::vector<TunnelSocket> result;
    for(const auto& part:parts) {
        const auto size=settings.structure_size*part.size,height=settings.hub_height*part.height;
        const auto scale=part.scale*addon_scale(settings,part.kind);
        const auto frame=infrastructure_frame(part,settings);
        const auto append=[&](u32 id,std::string label,SkywaySample p) {
            p=infrastructure_sample(frame,p);
            result.push_back({{part.id,id},part.name+" / "+label,p.position,p.tangent,p.up,size*scale});
        };
        if(part.kind==InfrastructureKind::terminal) {
            auto p=detail::freestanding_terminal_path(size,height).front();p.tangent=mul(p.tangent,-1);
            append(1,"Entrance",p);
        } else if(part.kind==InfrastructureKind::joiner) {
            const auto ports=detail::joiner_sockets(size,height);
            append(1,"Inlet",ports[0]);append(2,"Left outlet",ports[1]);append(3,"Right outlet",ports[2]);
        }
    }
    return result;
}
content::Result<void> validate_infrastructure_connections(std::span<const InfrastructurePart> parts,InfrastructureSettings settings) {
    const auto sockets=infrastructure_sockets(parts,settings);
    std::set<std::pair<u32,u32>> used;
    for(const auto& part:parts)for(const auto ref:{part.socket_a,part.socket_b}) {
        if(!ref) {if(ref.socket)return error("A ground endpoint cannot name a socket");continue;}
        if(part.kind!=InfrastructureKind::skyway)return error("Only tunnel endpoints can attach to sockets");
        if(std::ranges::find(sockets,ref,&TunnelSocket::id)==sockets.end())return error("Unknown terminal/joiner socket");
        if(!used.emplace(ref.part,ref.socket).second)return error("This tunnel socket is already occupied");
    }
    for(const auto& part:parts)if(part.kind==InfrastructureKind::skyway) {
        if((part.socket_a&&part.terminal_a)||(part.socket_b&&part.terminal_b))return error("A socket-connected end cannot also own a dispersal fitting");
        if(auto curve=TunnelCurve::create(part,parts,settings);!curve)return std::unexpected(curve.error());
    }
    return {};
}
content::Result<TunnelCurve> TunnelCurve::create(const InfrastructurePart& part,std::span<const InfrastructurePart> parts,InfrastructureSettings settings) {
    if(!valid(settings)||!valid(part)||part.kind!=InfrastructureKind::skyway)return error("Invalid tunnel recipe or settings");
    const auto sockets=infrastructure_sockets(parts,settings);
    const auto find=[&](TunnelSocketRef ref)->const TunnelSocket* {
        const auto it=std::ranges::find(sockets,ref,&TunnelSocket::id);return it==sockets.end()?nullptr:&*it;
    };
    const auto* a=find(part.socket_a);const auto* b=find(part.socket_b);
    if((part.socket_a&&!a)||(part.socket_b&&!b))return error("Unknown terminal/joiner socket");
    const auto pa=a?a->position:mul(direction(part.location),skyway_endpoint_radius+part.altitude_a);
    const auto pb=b?b->position:mul(direction(part.end),skyway_endpoint_radius+part.altitude_b);
    const auto scale=part.scale*addon_scale(settings,part.kind);
    auto ground=SkywayCurve::create(location(pa),location(pb),skyway_default_rise*settings.skyway_height*part.height*scale,
        std::max(skyway_endpoint_radius,length(pa)),std::max(skyway_endpoint_radius,length(pb)));
    if(!ground)return std::unexpected(ground.error());
    TunnelCurve result(*ground);result.attached_=a||b;result.a_=pa;result.b_=pb;
    result.size_=settings.structure_size*part.size*scale*tunnel_width_coefficient(part.tunnel_class);
    result.size_a_=a?a->size:result.size_;result.size_b_=b?b->size:result.size_;
    result.rise_=skyway_default_rise*settings.skyway_height*part.height*scale;
    // Hermite endpoint derivatives point out of A and into B. Length scales
    // with separation, so rotating a socket produces a smooth approach bend.
    const auto distance=length(add(pb,mul(pa,-1)));
    result.da_=mul(a?a->outward:ground->sample(0).tangent,distance);
    result.db_=mul(b?mul(b->outward,-1):ground->sample(1).tangent,distance);
    result.up_a_=a?a->up:ground->sample(0).up;
    result.up_b_=b?b->up:ground->sample(1).up;
    result.bezier_=part.bezier_controls;
    return result;
}
std::vector<Vec3> TunnelCurve::initial_bezier_controls() const {
    const auto distance=length(add(b_,mul(a_,-1)))/3;
    return {add(a_,mul(sample(0).tangent,distance)),add(b_,mul(sample(1).tangent,-distance))};
}
Vec3 TunnelCurve::position(f32 t) const {
    if(t<=0)return a_;
    if(t>=1)return b_;
    if(bezier_) {
        // De Casteljau: bounded degree, no heap allocation while sampling.
        std::array<Vec3,16> points;std::size_t count=bezier_->size()+2;
        points[0]=a_;std::copy(bezier_->begin(),bezier_->end(),points.begin()+1);points[count-1]=b_;
        for(auto n=count;n>1;--n)for(std::size_t i=0;i+1<n;++i)
            points[i]=add(mul(points[i],1-t),mul(points[i+1],t));
        return points[0]; // Explicit controls: no hidden arch or surface projection.
    }
    const auto t2=t*t,t3=t2*t;
    auto p=add(add(mul(a_,2*t3-3*t2+1),mul(b_,-2*t3+3*t2)),
        add(mul(da_,t3-2*t2+t),mul(db_,t3-t2)));
    const auto radial=unit(add(mul(a_,1-t),mul(b_,t)));
    const auto arch=std::sin(std::numbers::pi_v<f32>*t);
    p=add(p,mul(radial,rise_*arch*arch));
    // Keep long chords above the terrain. Endpoints and their tangents are
    // unchanged; short regional corridors normally never reach this floor.
    return mul(unit(p),std::max(length(p),1.018F));
}
SkywaySample TunnelCurve::sample(f32 progress) const {
    if(!attached_&&!bezier_)return ground_.sample(progress);
    const auto t=std::clamp(progress,0.F,1.F);const auto p=position(t);
    const auto derivative=add(position(std::min(t+.0002F,1.F)),mul(position(std::max(t-.0002F,0.F)),-1));
    const auto tangent=bezier_?(length(derivative)>1e-8F?unit(derivative):ground_.sample(t).tangent):
        t==0?unit(da_):t==1?unit(db_):unit(derivative);
    auto up=length(p)>1e-7F?unit(p):ground_.sample(t).up;
    if(t<.25F)up=add(mul(up,smooth(t*4)),mul(up_a_,1-smooth(t*4)));
    if(t>.75F)up=add(mul(up,smooth((1-t)*4)),mul(up_b_,1-smooth((1-t)*4)));
    auto side=cross(up,tangent);
    if(length(side)<.001F)side=ground_.sample(t).side;
    side=unit(side);
    return {p,tangent,side,unit(cross(tangent,side))};
}
f32 TunnelCurve::size(f32 t) const {
    return t<.5F?size_a_+(size_-size_a_)*smooth(t*4):size_b_+(size_-size_b_)*smooth((1-t)*4);
}
TunnelArc::TunnelArc(const TunnelCurve& curve) {
    auto previous=curve.sample(0).position;
    for(std::size_t i=1;i<distances_.size();++i) {
        const auto p=curve.sample(f32(i)/128).position;
        distances_[i]=distances_[i-1]+std::hypot(p.x-previous.x,p.y-previous.y,p.z-previous.z);previous=p;
    }
}
f32 TunnelArc::distance(f32 t) const {
    if(!std::isfinite(t))return 0;
    const auto scaled=std::clamp(t,0.F,1.F)*128;
    const auto i=std::min(std::size_t(scaled),std::size_t{127});
    return distances_[i]+(distances_[i+1]-distances_[i])*(scaled-f32(i));
}
f32 TunnelArc::parameter(f32 d) const {
    if(!std::isfinite(d)||d<=0)return 0;
    if(d>=length())return 1;
    const auto it=std::lower_bound(distances_.begin()+1,distances_.end(),d);
    const auto i=std::size_t(it-distances_.begin());
    return (f32(i-1)+(d-distances_[i-1])/std::max(1e-12F,distances_[i]-distances_[i-1]))/128;
}
content::Result<std::vector<f32>> uniform_tunnel_supports(const TunnelArc& arc,f32 first,f32 last,f32 spacing,std::span<const f32> endpoints) {
    if(!std::isfinite(first)||!std::isfinite(last)||!std::isfinite(spacing)||first<=0||last>=1||first>=last||spacing<=0||
        std::ranges::any_of(endpoints,[](f32 p){return p!=0&&p!=1;}))return error("Uniform spacing needs only endpoint supports, 0 < first < last < 1, and positive spacing");
    const auto begin=arc.distance(first),length=arc.distance(last)-begin;
    const auto intervals=std::max(1.,std::ceil(double(length)/spacing));
    if(!std::isfinite(intervals)||intervals+1+double(endpoints.size())>64)return error("Too many supports (maximum 64); increase spacing");
    std::vector<f32> result(endpoints.begin(),endpoints.end());
    for(unsigned i=0;i<=unsigned(intervals);++i)
        result.push_back(i==0?first:i==unsigned(intervals)?last:arc.parameter(begin+length*f32(double(i)/intervals)));
    return result;
}
content::Result<std::vector<f32>> tunnel_support_positions(const InfrastructurePart& part,const TunnelCurve& curve) {
    if(!valid(part)||part.kind!=InfrastructureKind::skyway)return error("Expected a valid tunnel");
    if(part.scaffold_positions)return *part.scaffold_positions;
    if(part.scaffold_spacing>0){const auto range=tunnel_body_range(part,curve);return curve.scaffold_parameters(part.scaffold_spacing,range.x,range.y);}
    std::vector<f32> result;
    for(bool end:{false,true}) {
        if(end?bool(part.socket_b)||part.altitude_b>0:bool(part.socket_a)||part.altitude_a>0)
            for(f32 t:{.08F,.23F,.38F})result.push_back(end?1-t:t);
        else result.push_back(end?1.F:0.F);
    }
    return result;
}
Vec2 tunnel_body_range(const InfrastructurePart& part,const TunnelCurve& curve) {
    const TunnelArc arc(curve);
    const auto span=std::min(arc.length()*.18F,4*tunnel_inner_half_height*curve.size(.5F));
    return {part.terminal_a?arc.parameter(span):0.F,part.terminal_b?arc.parameter(arc.length()-span):1.F};
}
SkywaySample tunnel_terminal_sample(const InfrastructurePart& part,const TunnelCurve& curve,bool end,f32 progress) {
    const auto range=tunnel_body_range(part,curve);
    const auto at=[&](f32 t) {
        auto p=curve.sample(end?range.y+(1-range.y)*t:range.x*(1-t));
        const TunnelArc arc(curve);
        const auto length=end?arc.length()-arc.distance(range.y):arc.distance(range.x);
        p.position=add(p.position,mul(unit(p.position),length*tunnel_terminal_rise(part)*t*t));
        return p;
    };
    auto p=at(progress);
    p.tangent=unit(add(at(std::min(1.F,progress+.001F)).position,mul(at(std::max(0.F,progress-.001F)).position,-1)));
    if(!end)p.side=mul(p.side,-1);
    p.up=unit(cross(p.tangent,p.side));
    return p;
}
content::Result<std::vector<f32>> TunnelCurve::scaffold_parameters(f32 spacing,f32 begin,f32 end) const {
    if(!std::isfinite(spacing)||spacing<=0||!std::isfinite(begin)||!std::isfinite(end)||begin<0||end>1||begin>=end)
        return error("Invalid tunnel scaffold spacing");
    constexpr unsigned steps=128;
    std::array<f32,steps+1> distance{};auto previous=sample(begin).position;
    for(unsigned i=1;i<=steps;++i) {
        const auto point=sample(begin+(end-begin)*f32(i)/steps).position;
        distance[i]=distance[i-1]+length(add(point,mul(previous,-1)));previous=point;
    }
    const auto count=std::ceil(double(distance.back())/spacing);
    if(!std::isfinite(count)||count>64)return error("Too many tunnel scaffolds (maximum 64); increase Scaffold spacing");
    if(count<1)return error("Degenerate tunnel scaffold route");
    std::vector<f32> result;result.reserve(std::size_t(count));
    for(unsigned i=0;i<unsigned(count);++i) {
        const auto d=f32((i+.5)*distance.back()/count);
        const auto it=std::lower_bound(distance.begin()+1,distance.end(),d);
        const auto j=std::size_t(it-distance.begin());
        const auto fraction=(d-distance[j-1])/std::max(distance[j]-distance[j-1],1e-12F);
        result.push_back(begin+(end-begin)*(f32(j-1)+fraction)/steps);
    }
    return result;
}
}
