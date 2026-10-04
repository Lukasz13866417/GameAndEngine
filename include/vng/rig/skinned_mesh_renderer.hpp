#pragma once

#include <functional>
#include <optional>
#include <utility>

#include <vng/gfx/geometry.hpp>
#include <vng/render/graphics_state.hpp>
#include <vng/rig/rig.hpp>

namespace vng::render {

// Optional enhanced-render observations emitted by the supplied renderer.
struct SkinnedWorldPosition : gfx::Semantic<Vec3> {};
struct SkinnedWorldNormal : gfx::Semantic<Vec3> {};

template<class Position = gfx::Position, class Normal = gfx::Normal, class Color = gfx::Color>
struct GeometryFields final {
    Position position{};
    Normal normal{};
    Color color{};
};

struct SkinnedRendererOptions final {
    u32 max_influences{4}; // 4 or 8; excess source influences are an error
    bool lighting{true};  // simple directional lighting when normals exist
};

// A cone of light in world space, such as a flashlight's, added to a lit
// draw. It fades across the cone's edge, from `inner` to `outer` degrees off
// its axis, and with distance, to nothing at `range` metres.
struct SpotLight final {
    Vec3 position{};
    Vec3 direction{0, 0, -1};
    Vec3 color{1, 1, 1}; // linear, times its strength
    f32 inner{12}, outer{20};
    f32 range{8};
};

// Pose is borrowed only for the synchronous render/capture call. CPU poses
// are independent of GPU buffers and may be shared by several mesh bindings.
struct SkinnedDraw final {
    std::reference_wrapper<const rig::Pose> pose;
    rig::Transform transform{};
    bool depth_test{true};
    bool depth_write{true};
    CullMode cull{CullMode::none};
    std::optional<SpotLight> spot{}; // ignored unless the renderer lights the mesh
};

struct SkinnedRenderStats final {
    u32 draw_calls{};
    u32 palette_uploads{};
    u64 triangles{};
};

struct MakeSkinnedMeshRenderer final {
    template<class Device, class Binding, class... Options>
    [[nodiscard]] auto operator()(
        Device& device, const Binding& binding, Options&&... options) const
        -> decltype(make_backend_skinned_mesh_renderer(
            device, binding, std::forward<Options>(options)...))
    {
        return make_backend_skinned_mesh_renderer(
            device, binding, std::forward<Options>(options)...);
    }
};
inline constexpr MakeSkinnedMeshRenderer make_skinned_mesh_renderer{};

struct SkinnedMeshRendererBuilderFactory final {
    template<class Device>
    [[nodiscard]] auto operator()(Device& device) const
        -> decltype(make_backend_skinned_mesh_renderer_builder(device))
    {
        return make_backend_skinned_mesh_renderer_builder(device);
    }
};
inline constexpr SkinnedMeshRendererBuilderFactory skinned_mesh_renderer_builder{};

} // namespace vng::render
