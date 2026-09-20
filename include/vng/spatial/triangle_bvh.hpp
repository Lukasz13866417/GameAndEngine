#pragma once
#include <vng/core/types.hpp>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace vng::spatial {
// Directions need not be normalized. Affine transforms preserve the ray's t,
// which lets local-space queries retain camera near/far clipping distances.
struct Ray3 {
    std::array<double,3> origin{}, direction{};
    double minimum{}, maximum{std::numeric_limits<double>::infinity()};
};
struct RayHit { double distance{}; u32 triangle{}; };
struct QueryStats { std::size_t bounds_tests{}, triangle_tests{}; };

// Immutable, owning local-space geometry snapshot. Median-split bounding-volume
// hierarchy; no editor, GPU, scene identity or global cache dependencies.
class TriangleBvh {
public:
    using Triangle = std::array<u32,3>;
    // Invalid indices/nonfinite positions throw invalid_argument.
    TriangleBvh(std::span<const Vec3>, std::span<const Triangle>);
    [[nodiscard]] std::optional<RayHit> intersect(const Ray3&, QueryStats* = nullptr) const;
private:
    struct Bounds { std::array<double,3> minimum{}, maximum{}; };
    struct Node { Bounds bounds; u32 first{}, count{}, left{}, right{}; };
    std::vector<Vec3> vertices_;
    std::vector<Triangle> triangles_;
    std::vector<u32> order_;
    std::vector<Node> nodes_;
    u32 build(u32 first,u32 count);
};
} // namespace vng::spatial
