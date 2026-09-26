#pragma once
#include <vng/content/vmesh.hpp>
#include <span>
#include <string>
#include <vector>

// Offline, original geometry for the skyway departure's voyage into space.
// Kilometre units; craft-like parts face -Z with +Y up, like the ships.
namespace example::space {

// A level, graded area where the Moon's relief is flattened for building.
struct MoonSite {
    vng::Vec3 direction;
    vng::f32 radius_km{8};
};

// A flight corridor across the Moon, in kilometres along a great circle
// (through `origin`, heading `along`) and across it. The patch covers what a
// flight along the corridor sees; the strip covers where it hugs the ground.
struct MoonCorridor {
    vng::Vec3 origin, along;
    struct Span { vng::f32 begin, end, half_width; };
    Span patch{-300, 600, 150}, strip{-60, 170, 30};
};

// The Moon's relief and albedo as pure functions of direction, shared by its
// mesh and by scene authoring that flies just above or builds on the surface.
// The familiar maria face `near_side` (toward Earth). Craters come in three
// sizes, each only where the mesh resolves it: large ones everywhere, tens of
// kilometres across in the patch, and small bowls in the strip.
class MoonSurface final {
public:
    static constexpr vng::f32 radius = 1737;
    MoonSurface(MoonCorridor corridor, vng::Vec3 near_side, std::vector<MoonSite> sites = {});
    [[nodiscard]] vng::Vec3 near_side() const noexcept { return near_side_; }
    [[nodiscard]] const MoonCorridor& corridor() const noexcept { return corridor_; }
    // A unit direction from kilometres along and across the corridor, and back.
    [[nodiscard]] vng::Vec3 direction(vng::f32 along, vng::f32 across) const;
    [[nodiscard]] vng::Vec2 coordinates(vng::Vec3 direction) const;
    // Kilometres inside a corridor span; negative outside it.
    [[nodiscard]] vng::f32 inside(const MoonCorridor::Span&, vng::Vec3 direction) const;
    // Kilometres above the mean radius, for a unit direction from the center.
    [[nodiscard]] vng::f32 height(vng::Vec3 direction) const;
    [[nodiscard]] vng::Vec3 point(vng::Vec3 direction, vng::f32 altitude = 0) const;
    // Linear RGB: bright highlands, dark maria, fresh bright ejecta.
    [[nodiscard]] vng::Vec3 albedo(vng::Vec3 direction) const;

private:
    enum class Tier : vng::u8 { global, patch, strip };
    // `soft` widens rims that would otherwise be narrower than the mesh spacing.
    struct Crater { vng::Vec3 center; vng::f32 radius, depth, soft; bool fresh; Tier tier; };
    struct Basin { vng::Vec3 center; vng::f32 radius; };
    struct Sample { vng::f32 height, floor, bright, mare; };
    [[nodiscard]] Sample relief(vng::Vec3 direction) const;
    [[nodiscard]] std::size_t bucket(vng::Vec3 direction) const;
    MoonCorridor corridor_;
    vng::Vec3 center_, along_, across_, near_side_;
    std::vector<MoonSite> sites_;
    std::vector<vng::f32> site_levels_;
    std::vector<Crater> craters_;
    std::vector<Basin> basins_;
    // Craters indexed by 2-degree latitude/longitude cells they can affect.
    std::vector<std::vector<vng::u32>> buckets_;
};

// The whole Moon in three nested grids: about 75 km apart everywhere, 5 km
// in the corridor's patch and 0.6 km in its strip. Each finer grid fills a
// hole in the coarser one, its rim lying exactly on the coarser triangles,
// so there are no seams. Illustrated lighting like Earth's; the night side
// keeps a faint earthshine.
[[nodiscard]] vng::content::vmesh::Document moon_mesh(const MoonSurface&);
// Domes, landing pads, towers and a mass driver on a graded site, in the
// Moon's own coordinates so the base shares the Moon's instance transform.
[[nodiscard]] vng::content::vmesh::Document lunar_base(const MoonSurface&, vng::Vec3 site, vng::Vec3 rail_heading);
// An orbital gateway along +X: hub spindle, docking arms, a construction dock
// and solar wings. Its habitat ring is a separate blueprint so it can spin.
[[nodiscard]] vng::content::vmesh::Document gateway_station();
// A spoked habitat ring around +X; spinning it is a rotation about local X.
[[nodiscard]] vng::content::vmesh::Document habitat_ring();
[[nodiscard]] inline constexpr vng::f32 habitat_ring_radius() { return 6.2F; }
// An unlit, very bright sphere of radius one: warp flashes, a drive seen from
// afar, or the white-hot core that makes a distant sun blaze through bloom.
[[nodiscard]] vng::content::vmesh::Document glow_sphere(std::string name, vng::Vec3 color, vng::f32 emission);
// A small faceted rock fragment of radius about one, in a few hundred vertices.
[[nodiscard]] vng::content::vmesh::Document fragment(vng::u32 seed);
// Unlit exhaust cones of unit length behind each nozzle (+Z), `hot` at the
// nozzle and fading to black toward the tip.
[[nodiscard]] vng::content::vmesh::Document engine_plume(std::span<const vng::Vec3> nozzles, vng::f32 radius,
                                                         vng::Vec3 hot = {.6F,.88F,1.F});
}
