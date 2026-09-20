#pragma once
#include <vng/core/types.hpp>
#include <array>
#include <cmath>
#include <numbers>

namespace editor_example::rotation_math {
// Small CPU-only orientation math. Matrices here are row-major doubles;
// persisted/rendered instance angles remain degrees with Rz * Ry * Rx.
using Matrix = std::array<std::array<double, 3>, 3>;
inline Matrix multiply(const Matrix& a,const Matrix& b) {
    Matrix m{};
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)for(unsigned k=0;k<3;++k)m[r][c]+=a[r][k]*b[k][c];
    return m;
}
inline Matrix transpose(const Matrix& m) {
    Matrix result{};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)result[r][c]=m[c][r];return result;
}
inline vng::Vec3 apply(const Matrix& m,vng::Vec3 p) {
    vng::Vec3 result{};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)result[r]+=static_cast<float>(m[r][c]*p[c]);return result;
}
inline constexpr double radians = std::numbers::pi / 180.;
inline Matrix matrix(vng::Vec3 angles) {
    const auto sx=std::sin(angles.x*radians), cx=std::cos(angles.x*radians);
    const auto sy=std::sin(angles.y*radians), cy=std::cos(angles.y*radians);
    const auto sz=std::sin(angles.z*radians), cz=std::cos(angles.z*radians);
    return {{{cz*cy, cz*sy*sx-sz*cx, cz*sy*cx+sz*sx},
             {sz*cy, sz*sy*sx+cz*cx, sz*sy*cx-cz*sx}, {-sy, cy*sx, cy*cx}}};
}
inline vng::Vec3 direction(vng::Vec3 angles, vng::Vec3 local) {
    const auto m=matrix(angles); vng::Vec3 result{};
    for (unsigned row=0; row<3; ++row)
        for (unsigned column=0; column<3; ++column) result[row]+=static_cast<float>(m[row][column]*local[column]);
    return result;
}
inline vng::Vec3 near(vng::Vec3 angles, vng::Vec3 reference) {
    for (unsigned i=0; i<3; ++i) {
        angles[i]=reference[i]+std::remainder(angles[i]-reference[i],360.F);
        if (angles[i] < -360 || angles[i] > 360) angles[i]=std::remainder(angles[i],360.F);
    }
    return angles;
}
inline vng::Vec3 euler(const Matrix& m, vng::Vec3 reference) {
    const auto cy=std::hypot(m[0][0],m[1][0]);
    const auto y=std::atan2(-m[2][0],cy);
    // At gimbal lock retain the reference Z angle instead of inventing a spin.
    const auto z=cy>1e-8 ? std::atan2(m[1][0],m[0][0]) : reference.z*radians;
    const auto x=cy>1e-8 ? std::atan2(m[2][1],m[2][2])
        : std::atan2(-m[1][2],m[1][1])+std::copysign(1.,y)*z;
    auto a=near({static_cast<float>(x/radians),static_cast<float>(y/radians),static_cast<float>(z/radians)},reference);
    auto b=near({static_cast<float>(x/radians+180),static_cast<float>(180-y/radians),static_cast<float>(z/radians+180)},reference);
    const auto distance=[&](vng::Vec3 value) {
        double sum{}; for(unsigned i=0;i<3;++i) { const double d=value[i]-reference[i]; sum+=d*d; } return sum;
    };
    return distance(a)<=distance(b) ? a : b;
}
// Intrinsic/body-axis turn: R_new = R_start * axis_angle(local_axis, angle).
// Compute from the captured start every time, never accumulate Euler increments.
inline vng::Vec3 turn(vng::Vec3 start, vng::Vec3 axis, double degrees) {
    degrees=std::remainder(degrees,360.);
    if (degrees==0) return start;
    const auto c=std::cos(degrees*radians), s=std::sin(degrees*radians), t=1-c;
    const double x=axis.x,y=axis.y,z=axis.z;
    const Matrix delta{{{t*x*x+c,t*x*y-s*z,t*x*z+s*y},
                        {t*x*y+s*z,t*y*y+c,t*y*z-s*x},
                        {t*x*z-s*y,t*y*z+s*x,t*z*z+c}}};
    const auto original=matrix(start); Matrix result{};
    for(unsigned r=0;r<3;++r) for(unsigned col=0;col<3;++col)
        for(unsigned k=0;k<3;++k) result[r][col]+=original[r][k]*delta[k][col];
    return euler(result,start);
}
} // namespace editor_example::rotation_math
