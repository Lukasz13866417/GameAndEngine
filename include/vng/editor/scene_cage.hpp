#pragma once
#include <vng/core/types.hpp>
#include <array>
#include <concepts>
#include <string>
#include <vector>

namespace vng::editor {
enum class CageElement { vertex, edge, face };
// Value-only scene manipulation description. Providers retain data/validation;
// tools render/pick these handles and return their stable IDs with new positions.
// No mesh, window, graphics backend, callbacks or object pointers are required.
struct ScenePoint {
    u32 id{};
    Vec3 position{}; // world space
    std::string label;
};
struct SceneCage {
    u64 object{}; // nonzero identity, unique among simultaneously displayed cages
    std::string label;
    u32 primary_point{}; // Outline selection focuses this handle; anchors the label.
    std::vector<ScenePoint> points; // IDs unique within this cage
    std::vector<std::array<u32,2>> edges; // indices into points, not handle IDs
    std::vector<std::vector<u32>> faces; // ordered polygon boundaries, indices into points
};
template<class T>
concept SceneCageProvider = requires(const T& value) {
    { value.scene_cage() } -> std::same_as<SceneCage>;
};
} // namespace vng::editor
