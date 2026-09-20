#pragma once

#include <vng/core/types.hpp>
#include <array>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace example::earth {

struct CloudSettings {
    vng::f32 coverage{1}, puff_size{1}, spiral_size{1};
    vng::f32 altitude{1}, relief{1};
    vng::f32 edge_scatter{1}; // 0: original banks; larger: increasingly dispersed fringes.
    bool visible{true};
    friend bool operator==(const CloudSettings&,const CloudSettings&)=default;
};
[[nodiscard]] bool valid(const CloudSettings&);

// Stable blueprint-local identities, independent of tessellation and settings.
// Location is longitude/latitude in degrees; names describe the initial layout.
struct CloudFormation {
    vng::u32 id;
    std::string name;
    vng::Vec2 location;
};
[[nodiscard]] std::span<const CloudFormation> cloud_catalog();

// Offline particle authoring. The asset generator samples the overlapping
// kernels into one opaque cloud mesh; no sprites, alpha sorting or live sim.
class CloudParticles final {
    // Flatten about the existing mid-height, then bring the entire flattened
    // layer 30% closer to the unit-radius ocean. This preserves terrain clearance.
    static constexpr vng::f32 altitude_scale=.7F;
    static constexpr vng::f32 relief_scale=.5F;
    static constexpr vng::f32 reference_relief=.006F;
    static constexpr vng::f32 base_radius=1.F+altitude_scale*(.019F+(1.F-relief_scale)*reference_relief);
    static constexpr vng::f32 height_scale=altitude_scale*relief_scale;
public:
    struct Particle {
        vng::Vec3 center; // Unit-sphere direction.
        vng::f32 radius; // Chord distance on the unit sphere.
        vng::f32 strength;
        vng::f32 height;
        vng::u32 formation{};
        friend bool operator==(const Particle&,const Particle&)=default;
    };
    struct Sample {
        vng::f32 density{};
        vng::f32 radius{base_radius};
        vng::Vec3 normal;
    };
    static constexpr vng::f32 boundary=.18F;

    explicit CloudParticles(CloudSettings = {});
    // Zero samples all formations; a nonzero ID isolates one editable formation.
    [[nodiscard]] Sample sample(vng::Vec3 direction, vng::u32 formation = 0) const;
    [[nodiscard]] std::span<const Particle> particles() const { return particles_; }
    struct Bounds {vng::Vec2 minimum,maximum;}; // unwrapped lon/lat degrees
    [[nodiscard]] Bounds bounds(vng::u32 formation) const;

private:
    CloudSettings settings_;
    vng::u32 formation_{};
    static constexpr int divisions=20;
    std::vector<Particle> particles_;
    // Compact-support kernels are binned by their 3D bounds. Sampling a mesh
    // vertex only visits nearby puffs; there are no longitude/pole special cases.
    std::array<std::vector<vng::u32>,divisions*divisions*divisions> bins_;
    static int cell(vng::f32 value);
    void add_particle(vng::Vec2 lon_lat,vng::f32 angular_radius,vng::f32 strength,vng::f32 height);
};

} // namespace example::earth
