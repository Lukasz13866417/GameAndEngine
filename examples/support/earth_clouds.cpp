#include "earth_clouds.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace example::earth {
namespace {
using namespace vng;
constexpr f32 radians=std::numbers::pi_v<f32>/180.F;
constexpr f32 puff_scale=1.15F;
constexpr f32 spiral_scale=1.25F;
Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 mul(Vec3 a,f32 b){return {a.x*b,a.y*b,a.z*b};}
f32 dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 unit(Vec3 p){return mul(p,1.F/std::sqrt(dot(p,p)));}
Vec3 direction(Vec2 p) {
    return {std::sin(p.x*radians)*std::cos(p.y*radians),std::sin(p.y*radians),
            std::cos(p.x*radians)*std::cos(p.y*radians)};
}
struct Random {
    u32 state=0xEA47138U;
    f32 next() {
        state^=state<<13;state^=state>>17;state^=state<<5;
        return static_cast<f32>(state>>8)/16777216.F;
    }
    f32 range(f32 low,f32 high){return low+(high-low)*next();}
};
}

std::span<const CloudFormation> cloud_catalog() {
    static const std::array formations{
        CloudFormation{1,"North Atlantic bank",{-42,45}},
        CloudFormation{2,"Central Atlantic bank",{-35,39}},
        CloudFormation{3,"South Atlantic bank",{-19,-28}},
        CloudFormation{4,"Eastern Pacific bank",{-115,22}},
        CloudFormation{5,"North Pacific bank",{-148,48}},
        CloudFormation{6,"South Pacific bank",{-136,-27}},
        CloudFormation{7,"Southeastern Pacific bank",{-88,-37}},
        CloudFormation{8,"Equatorial Atlantic bank",{-10,4}},
        CloudFormation{9,"Indian Ocean bank",{61,-18}},
        CloudFormation{10,"Central Asia bank",{75,47}},
        CloudFormation{11,"Western Pacific bank",{142,22}},
        CloudFormation{12,"Southern Ocean bank",{175,-40}},
        CloudFormation{13,"Equatorial Pacific bank",{155,-8}},
        CloudFormation{14,"Northern Europe bank",{25,57}},
        CloudFormation{15,"Atlantic spiral",{-46,26}},
        CloudFormation{16,"Indian Ocean spiral",{79,-34}},
        CloudFormation{17,"Pacific spiral",{-161,-12}}};
    return formations;
}

int CloudParticles::cell(f32 value) {
    return std::clamp(static_cast<int>(std::floor((value+1.F)*static_cast<f32>(divisions)*.5F)),0,divisions-1);
}

void CloudParticles::add_particle(Vec2 lon_lat,f32 angular_radius,f32 strength,f32 height) {
    const auto n=direction(lon_lat);
    const auto radius=2.F*std::sin(angular_radius*puff_scale*settings_.puff_size*radians*.5F);
    const auto id=static_cast<u32>(particles_.size());
    particles_.push_back({n,radius,strength*settings_.coverage,height,formation_});
    for(int z=cell(n.z-radius);z<=cell(n.z+radius);++z)
        for(int y=cell(n.y-radius);y<=cell(n.y+radius);++y)
            for(int x=cell(n.x-radius);x<=cell(n.x+radius);++x)
                bins_[static_cast<std::size_t>((z*divisions+y)*divisions+x)].push_back(id);
}

bool valid(const CloudSettings& s) {
    const auto range=[](f32 v,f32 lo,f32 hi){return std::isfinite(v)&&v>=lo&&v<=hi;};
    return range(s.coverage,.25F,2.F)&&range(s.puff_size,.5F,1.8F)&&range(s.spiral_size,.5F,1.8F)&&
        range(s.altitude,1.F,3.F)&&range(s.relief,0.F,2.F)&&range(s.edge_scatter,0.F,2.F);
}
CloudParticles::CloudParticles(CloudSettings settings):settings_(settings) {
    if(!valid(settings_))throw std::invalid_argument("Invalid Earth cloud settings");
    if(!settings_.visible)return;
    Random random;
    // A separate stream preserves all original puff identities and properties:
    // changing scatter must not reroll the weather or change its height.
    Random scatter{0xC10D5EEDU};
    const auto emit=[&](Vec2 p,f32 radius,f32 strength,f32 height,f32 fringe,Vec2 outward) {
        const auto edge=std::clamp((fringe-.15F)/.85F,0.F,1.F);
        const auto amount=settings_.edge_scatter*edge*edge;
        const auto radial=scatter.range(.7F,1.7F),sideways=scatter.range(-1.2F,1.2F);
        if(amount==0) {add_particle(p,radius,strength,height);return;}
        // Drift in the local tangent plane, biased away from the cloud's body.
        // Quadratic falloff keeps the core joined and progressively opens gaps
        // between outer puffs. Compensate longitude so high-latitude banks do
        // not get a different physical scatter distance.
        const auto east=(outward.x*radial-outward.y*sideways)*radius*amount;
        const auto north=(outward.y*radial+outward.x*sideways)*radius*amount;
        p.x+=east/std::max(.15F,std::cos(p.y*radians));p.y+=north;
        const auto wisp=1.F-.22F*amount/(1.F+amount);
        add_particle(p,radius*wisp,strength,height);
    };
    struct Front {f32 length,width,angle;};
    constexpr std::array fronts{
        Front{23,3,18},Front{15,2,-18},Front{21,3,-30},Front{26,3,-20},
        Front{21,4,28},Front{24,3,-28},Front{18,3,35},Front{13,2,10},
        Front{22,3,-24},Front{19,3,12},Front{20,3,-26},Front{27,3,22},
        Front{17,2,15},Front{15,2,15}};
    for(const auto& f:fronts) {
        ++formation_;
        const auto center=cloud_catalog()[formation_-1].location;
        const auto c=std::cos(f.angle*radians),s=std::sin(f.angle*radians);
        for(f32 along=-f.length*.95F;along<f.length*.95F;along+=1.8F) {
            const auto envelope=std::sqrt(std::max(0.F,1-along*along/(f.length*f.length)));
            for(int row=-1;row<=1;++row) {
                if(random.next()<(row==0?.06F:.23F))continue;
                const auto u=along+random.range(-.65F,.65F);
                const auto v=f.width*.75F*std::sin(u/f.length*3.F)
                    +static_cast<f32>(row)*f.width*.65F*envelope+random.range(-.8F,.8F);
                const Vec2 p{center.x+(u*c-v*s)/std::cos(center.y*radians),center.y+u*s+v*c};
                const auto height=random.range(.007F,.014F);
                const auto strength=random.range(.8F,1.5F);
                const auto radius=random.range(1.3F,2.6F)*(.65F+.35F*envelope);
                const auto across=v-f.width*.75F*std::sin(u/f.length*3.F);
                const auto side=across/(f.width*.65F*std::max(.2F,envelope));
                const auto end=u/f.length;
                const auto fringe=std::max(std::abs(side),std::abs(end));
                const auto length=std::max(.0001F,std::hypot(end*.45F,side));
                const Vec2 outward{(end*.45F*c-side*s)/length,(end*.45F*s+side*c)/length};
                emit(p,radius,strength,height,fringe,outward);
            }
        }
    }
    // A storm is a dense, uneven cloud shield around a small eye, with several
    // unequal rainbands trailing out of it. It is not one constant-width curl.
    // These are still offline kernels, blended into the same ordinary mesh.
    Random storm{0x5702C10DU};
    for(const auto& cloud:cloud_catalog().subspan(fronts.size())) {
        formation_=cloud.id;
        const auto center=cloud.location;
        const auto scale=spiral_scale*settings_.spiral_size;
        const auto orientation=storm.range(-std::numbers::pi_v<f32>,std::numbers::pi_v<f32>);
        const auto handedness=center.y<0?1.F:-1.F;
        const auto stretch=storm.range(.92F,1.12F);
        const auto puff=[&](f32 angle,f32 radius,f32 size,f32 strength,f32 height,f32 fringe) {
            const auto east=std::cos(angle)*stretch,north=std::sin(angle)/stretch;
            const auto length=std::hypot(east,north);
            const Vec2 p{center.x+east*radius*scale/std::cos(center.y*radians),center.y+north*radius*scale};
            emit(p,size*scale,strength,height,fringe,{east/length,north/length});
        };
        // Broken curved clusters, not complete concentric rings. Unequal arcs
        // leave pockets of ocean between billows, including near the eye.
        // Two narrow, offset rows add thickness without filling the entire disk.
        for(int arm=0;arm<3;++arm) {
            for(f32 phase=0;phase<2.5F;phase+=.32F) {
                const auto radius=1.65F+phase*.82F;
                const auto angle=orientation+handedness*(static_cast<f32>(arm)*2.1F+phase);
                for(int row=0;row<2;++row) {
                    if(storm.next()<(row==0?.10F:.38F))continue;
                    const auto r=radius+static_cast<f32>(row)*.48F+storm.range(-.12F,.12F);
                    const auto size=storm.range(.54F,.86F);
                    const auto jittered_angle=angle+storm.range(-.055F,.055F);
                    const auto strength=storm.range(.8F,1.35F),height=storm.range(.008F,.014F);
                    puff(jittered_angle,r,size,strength,height,.2F+phase*.07F);
                }
            }
        }
        struct Band {f32 offset,length,start,strength;};
        constexpr std::array bands{Band{0,4.4F,4.4F,1},Band{2.15F,3.5F,4.7F,.82F},Band{4.65F,2.8F,5.F,.7F}};
        for(const auto& band:bands) {
            for(f32 phase=0;phase<band.length;) {
                const auto progress=phase/band.length;
                const auto radius=band.start*std::exp(phase*.23F);
                const auto bend=.09F*std::sin(phase*2.6F+band.offset)+.045F*std::sin(phase*7.1F+orientation);
                const auto angle=orientation+handedness*(band.offset+phase+bend);
                for(int row=0;row<2;++row) {
                    if(storm.next()<.025F+.27F*progress*progress)continue;
                    const auto cross=(static_cast<f32>(row)-.5F)*.95F*(1-.45F*progress)+storm.range(-.25F,.25F);
                    const auto size=storm.range(.9F,1.35F)*(1-.55F*progress);
                    const auto strength=storm.range(.95F,1.5F)*band.strength*(1-.25F*progress);
                    const auto jittered_angle=angle+storm.range(-.025F,.025F),height=storm.range(.007F,.013F);
                    puff(jittered_angle,radius+cross,size,strength,height,.2F+.8F*progress);
                }
                // Approximately constant spatial spacing, not constant angular
                // steps which turn the outer arms into uniformly spaced dots.
                phase+=.9F/radius;
            }
        }
    }
}

CloudParticles::Bounds CloudParticles::bounds(u32 formation) const {
    const auto anchor=cloud_catalog()[formation-1].location.x;
    Bounds result{{anchor,90},{anchor,-90}};
    for(const auto& p:particles_)if(p.formation==formation) {
        auto lon=std::atan2(p.center.x,p.center.z)/radians;
        lon+=360*std::round((anchor-lon)/360);
        const auto lat=std::asin(std::clamp(p.center.y,-1.F,1.F))/radians;
        const auto angle=2*std::asin(std::min(1.F,p.radius*.5F));
        const auto delta_lat=angle/radians;
        const auto ratio=std::sin(angle)/std::max(.000001F,std::cos(lat*radians));
        const auto delta_lon=std::abs(lat)+delta_lat>=90?180.F:std::asin(std::clamp(ratio,0.F,1.F))/radians;
        result.minimum.x=std::min(result.minimum.x,lon-delta_lon);result.maximum.x=std::max(result.maximum.x,lon+delta_lon);
        result.minimum.y=std::min(result.minimum.y,lat-delta_lat);result.maximum.y=std::max(result.maximum.y,lat+delta_lat);
    }
    return result;
}

CloudParticles::Sample CloudParticles::sample(Vec3 n,u32 formation) const {
    f32 density{},weighted_height{};
    Vec3 gradient{},height_gradient{};
    const auto index=static_cast<std::size_t>((cell(n.z)*divisions+cell(n.y))*divisions+cell(n.x));
    for(const auto id:bins_[index]) {
        const auto& p=particles_[id];
        if(formation && p.formation!=formation)continue;
        const Vec3 delta{n.x-p.center.x,n.y-p.center.y,n.z-p.center.z};
        const auto inverse_radius=1.F/(p.radius*p.radius);
        const auto q=dot(delta,delta)*inverse_radius;
        if(q>=1)continue;
        const auto t=1-q,weight=p.strength*t*t*t;
        const auto derivative=mul(delta,-6.F*p.strength*t*t*inverse_radius);
        density+=weight;weighted_height+=weight*p.height;
        gradient=add(gradient,derivative);height_gradient=add(height_gradient,mul(derivative,p.height));
    }
    const auto denominator=.7F+density;
    const auto relief=height_scale*settings_.relief;
    const auto radius=1.F+(base_radius-1.F)*settings_.altitude+relief*weighted_height/denominator;
    // Derivative of the height ratio, projected onto the tangent plane. Softened
    // normals reveal billows without making the puffs look like shiny beads.
    auto slope=mul(add(mul(height_gradient,denominator),mul(gradient,-weighted_height)),relief/(denominator*denominator));
    slope=add(slope,mul(n,-dot(slope,n)));
    return {density,radius,unit(add(n,mul(slope,-.6F/radius)))};
}

} // namespace example::earth
