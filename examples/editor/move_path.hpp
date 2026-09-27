#pragma once
#include <vng/core/types.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace editor_example {
// CPU-only, blueprint-local polyline constraint. Uniform parameter samples
// describe a curve without giving UI tools a reference to the recipe/document.
struct MovePath {
    std::vector<vng::Vec3> points;
    [[nodiscard]] bool valid() const {
        return points.size()>=2&&points.size()<=4097&&std::ranges::all_of(points,[](auto p){
            return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);
        })&&std::ranges::any_of(points,[&](auto p){return p!=points.front();});
    }
    [[nodiscard]] vng::Vec3 sample(vng::f32 t) const {
        if(points.size()<2||!std::isfinite(t))return {};
        const auto index=std::clamp(t,0.F,1.F)*vng::f32(points.size()-1);
        const auto i=std::min(std::size_t(index),points.size()-2);
        vng::Vec3 result{};
        for(unsigned c=0;c<3;++c)result[c]=points[i][c]+(points[i+1][c]-points[i][c])*(index-vng::f32(i));
        return result;
    }
    [[nodiscard]] vng::f32 parameter(vng::Vec3 p) const {
        double nearest=std::numeric_limits<double>::infinity();vng::f32 result{};
        for(std::size_t i=1;i<points.size();++i) {
            const auto a=points[i-1],b=points[i];double length{},projection{};
            for(unsigned c=0;c<3;++c){const double d=double(b[c])-a[c];length+=d*d;projection+=(double(p[c])-a[c])*d;}
            const auto fraction=length>1e-20?std::clamp(projection/length,0.,1.):0.;
            double distance{};
            for(unsigned c=0;c<3;++c){const auto d=double(p[c])-a[c]-(double(b[c])-a[c])*fraction;distance+=d*d;}
            if(distance<nearest){nearest=distance;result=vng::f32((double(i-1)+fraction)/double(points.size()-1));}
        }
        return result;
    }
};
}
