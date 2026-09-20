#include <vng/spatial/triangle_bvh.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace vng::spatial {
namespace {
using Vector=std::array<double,3>;
Vector subtract(Vec3 a,Vec3 b) {return {double(a.x)-b.x,double(a.y)-b.y,double(a.z)-b.z};}
Vector cross(Vector a,Vector b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
double dot(Vector a,Vector b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
std::optional<double> triangle(const Ray3& ray,Vec3 a,Vec3 b,Vec3 c) {
    const auto ab=subtract(b,a),ac=subtract(c,a),p=cross(ray.direction,ac);
    const auto determinant=dot(ab,p);
    const auto scale=std::sqrt(dot(ab,ab)*dot(ac,ac)*dot(ray.direction,ray.direction));
    if(!std::isfinite(scale)||scale==0||std::abs(determinant)<=scale*1e-12)return {};
    Vector from{};for(unsigned i=0;i<3;++i)from[i]=ray.origin[i]-a[i];
    const auto u=dot(from,p)/determinant;
    if(u< -1e-10||u>1+1e-10)return {};
    const auto q=cross(from,ab);const auto v=dot(ray.direction,q)/determinant;
    if(v< -1e-10||u+v>1+1e-10)return {};
    const auto distance=dot(ac,q)/determinant;
    return std::isfinite(distance)&&distance>=ray.minimum&&distance<=ray.maximum ? std::optional{distance}:std::nullopt;
}
}
TriangleBvh::TriangleBvh(std::span<const Vec3> vertices,std::span<const Triangle> triangles)
    :vertices_(vertices.begin(),vertices.end()),triangles_(triangles.begin(),triangles.end()) {
    if(triangles.size()>std::numeric_limits<u32>::max()/2)throw std::invalid_argument("BVH is too large");
    for(auto p:vertices)for(unsigned a=0;a<3;++a)if(!std::isfinite(p[a]))throw std::invalid_argument("Nonfinite BVH position");
    for(auto face:triangles)for(auto v:face)if(v>=vertices.size())throw std::invalid_argument("BVH triangle index outside geometry");
    order_.resize(triangles.size());std::iota(order_.begin(),order_.end(),0U);
    nodes_.reserve(triangles.size()*2);
    if(!triangles.empty())build(0,static_cast<u32>(triangles.size()));
}
u32 TriangleBvh::build(u32 first,u32 count) {
    const auto id=static_cast<u32>(nodes_.size());nodes_.push_back({});
    auto& bounds=nodes_[id].bounds;
    bounds.minimum.fill(std::numeric_limits<double>::infinity());
    bounds.maximum.fill(-std::numeric_limits<double>::infinity());
    for(u32 i=first;i<first+count;++i)for(auto v:triangles_[order_[i]])for(unsigned a=0;a<3;++a) {
        bounds.minimum[a]=std::min(bounds.minimum[a],double(vertices_[v][a]));
        bounds.maximum[a]=std::max(bounds.maximum[a],double(vertices_[v][a]));
    }
    if(count<=8) {nodes_[id].first=first;nodes_[id].count=count;return id;}
    unsigned axis{};for(unsigned a=1;a<3;++a)
        if(bounds.maximum[a]-bounds.minimum[a]>bounds.maximum[axis]-bounds.minimum[axis])axis=a;
    const auto middle=first+count/2;
    const auto center=[&](u32 face) {const auto f=triangles_[face];return double(vertices_[f[0]][axis])+vertices_[f[1]][axis]+vertices_[f[2]][axis];};
    std::nth_element(order_.begin()+first,order_.begin()+middle,order_.begin()+first+count,
        [&](u32 a,u32 b){const auto x=center(a),y=center(b);return x==y?a<b:x<y;});
    const auto left=build(first,middle-first),right=build(middle,first+count-middle);
    nodes_[id].left=left;nodes_[id].right=right;return id;
}
std::optional<RayHit> TriangleBvh::intersect(const Ray3& source,QueryStats* statistics) const {
    if(nodes_.empty()||std::isnan(source.minimum)||std::isnan(source.maximum)||source.minimum>source.maximum)return {};
    for(unsigned a=0;a<3;++a)if(!std::isfinite(source.origin[a])||!std::isfinite(source.direction[a]))return {};
    auto ray=source;std::optional<RayHit> result;
    const auto intersects=[&](const Bounds& bounds) {
        if(statistics)++statistics->bounds_tests;
        auto low=ray.minimum,high=ray.maximum;
        for(unsigned a=0;a<3;++a) {
            // Include the triangle predicate's edge tolerance, including thin/flat bounds.
            const auto pad=std::max(1e-12,(bounds.maximum[a]-bounds.minimum[a])*1e-9);
            const auto minimum=bounds.minimum[a]-pad,maximum=bounds.maximum[a]+pad;
            if(ray.direction[a]==0) {if(ray.origin[a]<minimum||ray.origin[a]>maximum)return false;continue;}
            auto x=(minimum-ray.origin[a])/ray.direction[a],y=(maximum-ray.origin[a])/ray.direction[a];
            if(x>y)std::swap(x,y);
            low=std::max(low,x);high=std::min(high,y);
            if(low>high)return false;
        }
        return true;
    };
    // Median splitting bounds depth by log2(triangle count). No per-query allocation.
    std::array<u32,64> stack{};std::size_t size=1;
    while(size) {
        const auto& node=nodes_[stack[--size]];
        if(!intersects(node.bounds))continue;
        if(!node.count){stack[size++]=node.right;stack[size++]=node.left;continue;}
        for(u32 i=node.first;i<node.first+node.count;++i) {
            if(statistics)++statistics->triangle_tests;
            const auto id=order_[i];const auto f=triangles_[id];
            if(auto hit=triangle(ray,vertices_[f[0]],vertices_[f[1]],vertices_[f[2]]);hit&&(!result||*hit<result->distance)) {
                result=RayHit{*hit,id};ray.maximum=*hit;
            }
        }
    }
    return result;
}
} // namespace vng::spatial
