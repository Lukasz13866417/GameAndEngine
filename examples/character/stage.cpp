#include "stage.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace character {
namespace {
using namespace vng;
constexpr int tiles = 80;
constexpr f32 tile = Stage::tile;
constexpr Vec4 sky = Stage::sky;
// Beyond 10 m the tiles fade into the sky: lit by the renderer's fixed light,
// the fade colour lands exactly on the clear colour.
Mesh floor_mesh() {
    // An up-facing surface receives 0.75 * dot(up, light) + 0.25 of its colour.
    const f32 lit = .75F * .6F / std::sqrt(.3F * .3F + .6F * .6F + 1.F) + .25F;
    Mesh mesh{static_cast<std::size_t>(tiles * tiles * 4)};
    std::vector<gfx::TriangleFace> faces;
    const f32 start = -tile * tiles / 2;
    for (int row = 0; row < tiles; ++row)
        for (int column = 0; column < tiles; ++column) {
            const auto base = static_cast<u32>((row * tiles + column) * 4);
            const f32 x = start + tile * static_cast<f32>(column), z = start + tile * static_cast<f32>(row);
            const Vec4 color = (row + column) % 2 ? Vec4{.40F, .41F, .38F, 1} : Vec4{.47F, .48F, .45F, 1};
            const std::array<Vec3, 4> corners{Vec3{x, 0, z}, Vec3{x + tile, 0, z}, Vec3{x + tile, 0, z + tile}, Vec3{x, 0, z + tile}};
            for (u32 k = 0; k < 4; ++k) {
                const auto distance = std::hypot(corners[k].x, corners[k].z);
                const auto t = std::clamp((distance - 10.F) / 28.F, 0.F, 1.F);
                const auto fade = t * t * (3 - 2 * t);
                const auto mix = [&](f32 a, f32 b) { return a + (b - a) * fade; };
                auto& vertex = mesh.vertices()[base + k];
                vertex.set(gfx::Position{}, corners[k]);
                vertex.set(gfx::Normal{}, Vec3{0, 1, 0});
                vertex.set(gfx::Color{}, Vec4{mix(color.x, sky.x / lit), mix(color.y, sky.y / lit), mix(color.z, sky.z / lit), 1});
            }
            faces.push_back({base, base + 2, base + 1});
            faces.push_back({base, base + 3, base + 2});
        }
    mesh.faces() = std::move(faces);
    return mesh;
}
content::Diagnostic failure(const rig::Diagnostic& error) {
    content::Diagnostic out;
    out.code = content::ErrorCode::invalid_document;
    out.message = "Stage: " + error.message;
    return out;
}
} // namespace

content::Result<Stage> Stage::create() {
    rig::ArmatureBuilder bones;
    const auto bone = bones.add_bone("floor");
    auto armature = bones.build();
    if (!armature) return std::unexpected(failure(armature.error()));
    auto floor = rig::bind(floor_mesh(), *armature);
    if (!floor) return std::unexpected(failure(floor.error()));
    for (std::size_t v = 0; v < floor->mesh().vertex_count(); ++v)
        if (auto weight = floor->set_weights(v, {{bone, 1.F}}); !weight) return std::unexpected(failure(weight.error()));
    return Stage{std::move(*armature), std::move(*floor)};
}

f32 Stage::scroll(f32 speed, f32 seconds) { return -std::fmod(speed * seconds, 2 * tile); }
} // namespace character
