#pragma once

#include "sun_renderer.hpp"
#include <vng/shader/shader.hpp>

namespace example::sun::shaders {
struct Time : vng::gfx::Semantic<vng::f32> {};
struct Displacement : vng::gfx::Semantic<vng::f32> {};
struct Eye : vng::gfx::Semantic<vng::Vec3> {};
struct StrandHalo : vng::gfx::Semantic<vng::f32> {};
struct WhiteSpots : vng::gfx::Semantic<vng::f32> {};
struct Center : vng::gfx::Semantic<vng::Vec3> {};
struct Radius : vng::gfx::Semantic<vng::f32> {};
struct BillboardRight : vng::gfx::Semantic<vng::Vec3> {};
struct BillboardUp : vng::gfx::Semantic<vng::Vec3> {};
using Parameters = vng::gfx::Record<Time, Displacement, Eye, StrandHalo, WhiteSpots,
    Center, Radius, BillboardRight, BillboardUp>;
using Program = vng::shader::TypedGraphicsProgram<Parameters, vng::Mat3>;

[[nodiscard]] vng::resources::Result<Program> surface();
[[nodiscard]] vng::resources::Result<Program> corona();
[[nodiscard]] vng::resources::Result<Program> prominences();
} // namespace example::sun::shaders
