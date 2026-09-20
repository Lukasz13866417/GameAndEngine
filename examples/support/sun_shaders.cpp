#include "sun_shaders.hpp"

namespace example::sun::shaders {
namespace {
using namespace vng;
using dsl::field;
using Param = dsl::Expr<Parameters>;
using Outputs = shader::FragmentOutputs<shader::Color<0>>;
using SurfaceInputs = shader::VertexInputs<gfx::Position, SurfaceUV>;
using SurfaceOutputs = shader::VertexOutputs<shader::ClipPosition,
    shader::smooth<WorldPosition>, shader::smooth<SurfaceNormal>, shader::smooth<SurfaceUV>>;
using SurfaceFragments = shader::FragmentInputs<shader::smooth<WorldPosition>,
    shader::smooth<SurfaceNormal>, shader::smooth<SurfaceUV>>;

// Small bounded local motion, not a second rotation of the texture. Surface
// landmarks remain in sun-local coordinates while the whole body rotates.
dsl::Float2 flowing_uv(dsl::Float2 uv, dsl::Float time)
{
    const auto polar_fade = dsl::sin(uv.y() * 3.14159265F);
    return uv + dsl::vec2(dsl::sin(time * 0.09F) * 0.0009F,
        dsl::sin(time * 0.11F) * 0.0007F) * polar_fade;
}
dsl::Float3 sun_to_world(dsl::Float3 local, dsl::Float3x3 rotation, Param parameters)
{
    return parameters.get(Center{}) + (rotation * local) * parameters.get(Radius{});
}
template<class Vertex, class Fragment>
resources::Result<Program> finish(Vertex vertex, Fragment fragment)
{
    if (!vertex) return std::unexpected(resources::to_diagnostic(std::move(vertex.error())));
    if (!fragment) return std::unexpected(resources::to_diagnostic(std::move(fragment.error())));
    return resources::into_result(shader::link(std::move(*vertex), std::move(*fragment),
        shader::shared_arguments));
}
} // namespace

resources::Result<Program> surface()
{
    auto vertex = shader::vertex<SurfaceInputs, SurfaceOutputs>("solar_surface_vertex",
        [](auto& s, Param parameters, dsl::Float3x3 rotation) {
            const auto uv = s.input(SurfaceUV{});
            const auto time = parameters.get(Time{});
            const auto base = s.input(gfx::Position{});
            // Filter displacement to the authored sphere's vertex spacing.
            // Fade to mip zero at the poles: only the base map's pole row is
            // identical at every longitude, keeping duplicate cap tips joined.
            const auto lod = dsl::clamp(dsl::min(uv.y(), 1.0F - uv.y()) * 64.0F,
                0.0F, 1.0F) * 2.0F;
            const auto map = s.template sample_2d_lod<0>(flowing_uv(uv, time), lod);
            const auto height = (map.y() - 0.5F) * 0.013F
                + (map.x() - 0.5F) * 0.004F
                + dsl::sin(dsl::dot(base, Vec3{31.0F, 27.0F, 19.0F}) + time * 0.45F) * 0.001F;
            const auto world = sun_to_world(
                base * (1.0F + parameters.get(Displacement{}) * height), rotation, parameters);
            // Cofactors are proportional to inverse transpose (positive determinant).
            const auto x=rotation*Vec3{1,0,0},y=rotation*Vec3{0,1,0},z=rotation*Vec3{0,0,1};
            const auto normal=dsl::cross(y,z)*base.x()+dsl::cross(z,x)*base.y()+dsl::cross(x,y)*base.z();
            return s.output(field<shader::ClipPosition>(s.camera().project(world)),
                field<WorldPosition>(world), field<SurfaceNormal>(normal), field<SurfaceUV>(uv));
        });
    auto fragment = shader::fragment<SurfaceFragments, Outputs>("solar_surface_fragment",
        [](auto& s, Param parameters, dsl::Float3x3) {
            const auto time = parameters.get(Time{});
            const auto world = s.input(WorldPosition{});
            const auto uv = flowing_uv(s.input(SurfaceUV{}), time);
            const auto map = s.template sample_2d<0>(uv);
            // A second slowly advected sample evolves cells instead of merely
            // translating the entire image. Pole displacement stays bounded.
            const auto detail = s.template sample_2d<0>(uv + dsl::vec2(
                dsl::sin(time * 0.17F + map.y() * 8.0F) * 0.0012F,
                dsl::cos(time * 0.13F + map.w() * 7.0F) * 0.0006F));
            const auto grain = dsl::mix(map.x(), detail.x(), 0.32F);
            // Sample authored landmarks without local advection: arcade-root
            // footprints and the additional small hot knots stay sun-local.
            const auto landmarks = s.template sample_2d<0>(s.input(SurfaceUV{}));
            const auto activity = landmarks.z();
            const auto active_region = landmarks.w();
            // Fine turbulent emission covers the disk. Contrast belongs to
            // small fibrils and lanes, not broad ochre landmass-like regions.
            const auto temperature = dsl::clamp((map.y() - 0.24F) * 1.8F
                + (grain - 0.65F) * 0.65F, 0.0F, 1.0F);
            // G already spans a wide tonal range. Preserve those gradients
            // instead of thresholding most of the disk into two flat colors.
            // This response is separate from the authored white-hot knots.
            const auto body = dsl::clamp(0.08F + map.y() * 0.85F
                + (grain - 0.65F) * 0.2F, 0.0F, 1.0F);
            const auto hot = body * body;
            const auto spot_core = dsl::pow(activity, 5.0F);
            const auto normal = dsl::normalize(s.input(SurfaceNormal{}));
            const auto eye = dsl::normalize(parameters.get(Eye{}) - world);
            const auto mu = dsl::max(dsl::dot(normal, eye), 0.0F);
            const auto limb = 0.88F + 0.12F * dsl::sqrt(mu);
            // This is a false-color palette in linear HDR, before the shared
            // Reinhard/display transform: orange body, yellow fine highlights,
            // with white reserved for small active-region knots.
            const auto chroma = dsl::mix(Vec3{0.55F, 0.022F, 0.0002F},
                Vec3{17.0F, 0.72F, 0.006F}, hot)
                + Vec3{0.0F, 1.0F, 0.02F} * dsl::pow(body, 8.0F);
            const auto brightness = (1.0F - spot_core * 0.5F) * limb;
            const auto knots = dsl::pow(dsl::clamp(temperature * 1.5F, 0.0F, 1.0F), 3.0F);
            const auto footprint = dsl::clamp((active_region - 0.60F) / 0.40F, 0.0F, 1.0F);
            const auto active = dsl::pow(footprint, 4.0F) * (0.08F + knots * 0.92F);
            // Granulation alone never turns white: that energy belongs only
            // to the tighter authored footprints, including the small knots.
            const auto light = chroma * brightness
                + Vec3{24.0F, 20.0F, 13.0F} * (active * parameters.get(WhiteSpots{}));
            const auto radiance = dsl::vec4(light, 1.0F);
            s.observe(WorldPosition{}, world);
            s.observe(SurfaceNormal{}, normal);
            s.observe(Radiance{}, radiance);
            return s.output(field<shader::Color<0>>(radiance));
        });
    return finish(std::move(vertex), std::move(fragment));
}

resources::Result<Program> corona()
{
    using Inputs = shader::VertexInputs<gfx::Position>;
    using Varyings = shader::VertexOutputs<shader::ClipPosition, shader::smooth<SurfaceUV>>;
    using Fragments = shader::FragmentInputs<shader::smooth<SurfaceUV>>;
    auto vertex = shader::vertex<Inputs, Varyings>("solar_corona_vertex", [](auto& s, Param parameters, dsl::Float3x3) {
        const auto p = s.input(gfx::Position{});
        const auto world = parameters.get(Center{}) + (parameters.get(BillboardRight{}) * p.x()
            + parameters.get(BillboardUp{}) * p.y()) * parameters.get(Radius{});
        return s.output(field<shader::ClipPosition>(s.camera().project(world)), field<SurfaceUV>(p.xy()));
    });
    auto fragment = shader::fragment<Fragments, Outputs>("solar_corona_fragment",
        [](auto& s, Param parameters, dsl::Float3x3) {
            const auto p = s.input(SurfaceUV{});
            const auto r = dsl::sqrt(dsl::max(dsl::dot(p, p), 0.0001F));
            const auto direction = p / r;
            const auto time = parameters.get(Time{});
            const auto twist = dsl::sin(r * 4.0F + time * 0.07F) * 0.15F;
            const auto curved = dsl::vec2(direction.x() * dsl::cos(twist) - direction.y() * dsl::sin(twist),
                direction.x() * dsl::sin(twist) + direction.y() * dsl::cos(twist));
            const auto grain = s.template sample_2d<0>(curved * 0.37F
                + dsl::vec2(time * 0.002F + r * 0.18F, r * 0.29F));
            const auto fine = s.template sample_2d<0>(curved * 1.3F
                + dsl::vec2(time * -0.003F, r * 0.25F));
            const auto distance = dsl::max(r - 1.012F, 0.0F);
            const auto streamer = dsl::pow(grain.y() * 0.3F + grain.x() * 0.7F, 2.0F)
                * (0.5F + fine.x() * 0.5F);
            const auto fan = dsl::pow(0.5F + 0.5F * dsl::sin(
                direction.x() * 7.0F + direction.y() * 5.0F + time * 0.02F), 3.0F);
            const auto close = dsl::exp(-distance * 22.0F) * (0.13F + streamer * 0.23F);
            const auto far = dsl::exp(-distance * 11.0F) * streamer * (0.025F + fan * 0.028F);
            const auto rim = dsl::exp(-dsl::abs(r - 1.014F) * 95.0F) * (0.06F + streamer * 0.16F);
            const auto spicules = dsl::pow(dsl::clamp((fine.x() - 0.55F) * 2.2F,
                0.0F, 1.0F), 3.0F) * dsl::exp(-distance * 60.0F);
            // Red outer fringe and a tighter orange limb, rather than a dusty
            // yellow haze. The textured falloff avoids a uniform outline.
            const auto color = Vec3{3.8F, 0.016F, 0.0003F} * (close + far)
                + Vec3{5.0F, 0.28F, 0.002F} * rim
                + Vec3{7.0F, 0.35F, 0.002F} * spicules;
            return s.output(field<shader::Color<0>>(dsl::vec4(color, 1.0F)));
        });
    return finish(std::move(vertex), std::move(fragment));
}

resources::Result<Program> prominences()
{
    using Inputs = shader::VertexInputs<gfx::Position, gfx::Normal, SurfaceUV, TubeOffset>;
    using Varyings = shader::VertexOutputs<shader::ClipPosition, shader::smooth<SurfaceUV>,
        shader::smooth<TubeOffset>, shader::smooth<WorldPosition>>;
    using Fragments = shader::FragmentInputs<shader::smooth<SurfaceUV>, shader::smooth<TubeOffset>,
        shader::smooth<WorldPosition>>;
    auto vertex = shader::vertex<Inputs, Varyings>("solar_prominence_vertex",
        [](auto& s, Param parameters, dsl::Float3x3 rotation) {
            const auto uv = s.input(SurfaceUV{});
            const auto time = parameters.get(Time{});
            const auto movement = dsl::sin(uv.x() * 3.14159265F)
                * dsl::sin(time * 0.55F + uv.x() * 9.0F + uv.y() * 37.0F) * 0.005F;
            const auto local = s.input(gfx::Position{}) + s.input(gfx::Normal{})
                * (movement * parameters.get(Displacement{}))
                + s.input(TubeOffset{}) * (parameters.get(StrandHalo{}) * 9.0F);
            const auto p = sun_to_world(local, rotation, parameters);
            return s.output(field<shader::ClipPosition>(s.camera().project(p)), field<SurfaceUV>(uv),
                field<TubeOffset>((rotation * s.input(TubeOffset{})) * parameters.get(Radius{})), field<WorldPosition>(p));
        });
    auto fragment = shader::fragment<Fragments, Outputs>("solar_prominence_fragment",
        [](auto& s, Param parameters, dsl::Float3x3) {
            const auto uv = s.input(SurfaceUV{});
            const auto time = parameters.get(Time{});
            const auto wave = 0.5F + 0.5F * dsl::sin(uv.x() * 25.0F - time * 1.1F + uv.y() * 53.0F);
            const auto knots = dsl::pow(wave, 3.0F);
            const auto base = 0.68F + 0.32F * dsl::pow(
                0.5F + 0.5F * dsl::sin(uv.x() * 63.0F + uv.y() * 71.0F), 2.0F);
            const auto feet = dsl::exp(-uv.x() * 34.0F) + dsl::exp(-(1.0F - uv.x()) * 34.0F);
            const auto offset = s.input(TubeOffset{});
            const auto tube_normal = offset / dsl::sqrt(dsl::max(dsl::dot(offset, offset), 1.0e-12F));
            const auto sight = dsl::normalize(parameters.get(Eye{}) - s.input(WorldPosition{}));
            const auto softness = dsl::pow(dsl::abs(dsl::dot(tube_normal, sight)), 1.5F);
            const auto halo = parameters.get(StrandHalo{});
            // More overlapping filaments, with lower energy per filament and
            // a broader translucent envelope, read as one connected plasma arc.
            const auto attenuation = dsl::mix(0.42F, softness * 0.055F, halo);
            const auto tint = dsl::mix(Vec3{1.0F, 1.0F, 1.0F}, Vec3{1.0F, 0.72F, 0.5F}, halo);
            const auto color = (Vec3{4.0F, 0.08F, 0.001F} * base
                + Vec3{10.0F, 0.7F, 0.035F} * (knots * 0.5F + feet * 1.8F)) * attenuation * tint;
            s.observe(WorldPosition{}, s.input(WorldPosition{}));
            return s.output(field<shader::Color<0>>(dsl::vec4(color, 0.0F)));
        });
    return finish(std::move(vertex), std::move(fragment));
}
} // namespace example::sun::shaders
