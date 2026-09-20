#pragma once

#include <concepts>
#include <expected>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <vng/gfx/mesh.hpp>
#include <vng/rig/armature.hpp>

namespace vng::rig {

struct BindOptions final {
    Transform mesh_to_armature{};
};

struct Influence final {
    BoneId bone;
    f32 weight{};
};

struct PackedVertexInfluences final {
    UVec4 joints0{};
    UVec4 joints1{};
    Vec4 weights0{};
    Vec4 weights1{};
};

struct PackedInfluences final {
    u32 max_influences{};
    std::vector<PackedVertexInfluences> vertices;
};

struct VertexPruning final {
    std::size_t vertex{};
    u32 removed_influences{};
    f32 discarded_weight{};
};

struct PruningReport final {
    std::vector<VertexPruning> vertices;
    f32 maximum_discarded_weight{};
};

class SkinWeights final {
public:
    [[nodiscard]] static std::expected<SkinWeights, Diagnostic> create(
        std::size_t vertex_count, Armature armature, BindOptions options = {});
    [[nodiscard]] const Armature& armature() const noexcept { return armature_; }
    [[nodiscard]] std::size_t vertex_count() const noexcept { return influences_.size(); }
    [[nodiscard]] u64 revision() const noexcept { return revision_; }
    [[nodiscard]] const Transform& mesh_to_armature() const noexcept { return options_.mesh_to_armature; }
    [[nodiscard]] std::expected<std::span<const Influence>, Diagnostic> influences(std::size_t vertex) const;
    // Strong guarantee on failure. Duplicates are rejected; normalization is an
    // explicit authoring operation, never an implicit consequence of uploading.
    [[nodiscard]] std::expected<void, Diagnostic> set_weights(
        std::size_t vertex, std::span<const Influence> influences);
    [[nodiscard]] std::expected<void, Diagnostic> set_weights(
        std::size_t vertex, std::initializer_list<Influence> influences)
    {
        return set_weights(vertex, std::span<const Influence>{influences.begin(), influences.size()});
    }
    [[nodiscard]] std::expected<void, Diagnostic> validate() const;
    [[nodiscard]] std::expected<void, Diagnostic> normalize_weights();
    [[nodiscard]] std::expected<PruningReport, Diagnostic> prune_weights(u32 max_influences);
    [[nodiscard]] std::expected<PackedInfluences, Diagnostic> pack(u32 max_influences = 4) const;
    [[nodiscard]] std::expected<std::vector<Mat4>, Diagnostic> palette(const Pose& pose) const;
    [[nodiscard]] std::expected<std::vector<Vec3>, Diagnostic> deform_points(
        std::span<const Vec3> points, const Pose& pose) const;
    [[nodiscard]] std::expected<std::vector<Vec3>, Diagnostic> deform_normals(
        std::span<const Vec3> normals, const Pose& pose) const;
private:
    SkinWeights(std::size_t count, Armature armature, BindOptions options,
        Mat4 from_armature, std::vector<Mat4> inverse_rest)
        : armature_(std::move(armature)), options_(options),
          from_armature_(from_armature), inverse_rest_(std::move(inverse_rest)), influences_(count) {}
    Armature armature_;
    BindOptions options_;
    Mat4 from_armature_;
    std::vector<Mat4> inverse_rest_;
    std::vector<std::vector<Influence>> influences_;
    u64 revision_{};
};

namespace detail {
template<class T> struct is_mesh : std::false_type {};
template<class... Records> struct is_mesh<gfx::Mesh<Records...>> : std::true_type {};
[[nodiscard]] u64 next_snapshot_identity() noexcept;
}

template<class T>
concept MeshType = detail::is_mesh<std::remove_cvref_t<T>>::value;

template<MeshType Mesh>
class SkinBinding final {
public:
    using mesh_type = Mesh;
    [[nodiscard]] const Mesh& mesh() const
    {
        if (!mesh_) { throw std::logic_error("a moved-from skin binding has no mesh snapshot"); }
        return *mesh_;
    }
    [[nodiscard]] const Armature& armature() const noexcept { return weights_.armature(); }
    [[nodiscard]] const SkinWeights& weights() const noexcept { return weights_; }
    [[nodiscard]] u64 snapshot_identity() const noexcept { return mesh_ ? snapshot_identity_ : 0; }
    [[nodiscard]] std::expected<void, Diagnostic> set_weights(std::size_t vertex, std::span<const Influence> values)
    {
        return weights_.set_weights(vertex, values);
    }
    [[nodiscard]] std::expected<void, Diagnostic> set_weights(std::size_t vertex, std::initializer_list<Influence> values)
    {
        return weights_.set_weights(vertex, values);
    }
    [[nodiscard]] std::expected<void, Diagnostic> validate() const
    {
        if (!mesh_ || mesh_->vertex_count() != weights_.vertex_count()) {
            return std::unexpected(Diagnostic{ErrorCode::invalid_mesh,
                "skin binding has no compatible mesh snapshot (possibly moved-from)", {}});
        }
        return weights_.validate();
    }
    [[nodiscard]] std::expected<void, Diagnostic> normalize_weights() { return weights_.normalize_weights(); }
    [[nodiscard]] std::expected<PruningReport, Diagnostic> prune_weights(u32 limit) { return weights_.prune_weights(limit); }
    [[nodiscard]] std::expected<PackedInfluences, Diagnostic> pack(u32 limit = 4) const { return weights_.pack(limit); }
    [[nodiscard]] std::expected<std::vector<Mat4>, Diagnostic> palette(const Pose& pose) const { return weights_.palette(pose); }
    [[nodiscard]] std::expected<std::vector<Vec3>, Diagnostic> deform_points(std::span<const Vec3> points, const Pose& pose) const
    {
        return weights_.deform_points(points, pose);
    }
    [[nodiscard]] std::expected<std::vector<Vec3>, Diagnostic> deform_normals(std::span<const Vec3> normals, const Pose& pose) const
    {
        return weights_.deform_normals(normals, pose);
    }
private:
    SkinBinding(std::shared_ptr<const Mesh> mesh, SkinWeights weights)
        : mesh_(std::move(mesh)), weights_(std::move(weights)), snapshot_identity_(detail::next_snapshot_identity()) {}
    std::shared_ptr<const Mesh> mesh_;
    SkinWeights weights_;
    u64 snapshot_identity_;
    template<MeshType M>
    friend std::expected<SkinBinding<std::remove_cvref_t<M>>, Diagnostic> bind(M&&, Armature, BindOptions);
};

// Copy lvalues / move rvalues into an immutable owning geometry snapshot.
// Later edits to the original Mesh never invalidate this binding's vertex domain.
template<MeshType M>
[[nodiscard]] std::expected<SkinBinding<std::remove_cvref_t<M>>, Diagnostic> bind(
    M&& mesh, Armature armature, BindOptions options = {})
{
    if (auto valid = mesh.validate(); !valid) {
        return std::unexpected(Diagnostic{ErrorCode::invalid_mesh, valid.error().message, {}});
    }
    auto weights = SkinWeights::create(mesh.vertex_count(), std::move(armature), options);
    if (!weights) { return std::unexpected(weights.error()); }
    using Stored = std::remove_cvref_t<M>;
    return SkinBinding<Stored>{std::make_shared<const Stored>(std::forward<M>(mesh)), std::move(*weights)};
}

} // namespace vng::rig
