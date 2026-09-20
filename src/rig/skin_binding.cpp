#include <vng/rig/skin_binding.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace vng::rig {
namespace {
Diagnostic error(ErrorCode code, std::string message, std::optional<std::size_t> vertex = {})
{
    return {code, std::move(message), vertex};
}
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
}

u64 detail::next_snapshot_identity() noexcept
{
    static std::atomic<u64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

std::expected<SkinWeights, Diagnostic> SkinWeights::create(
    std::size_t vertex_count, Armature armature, BindOptions options)
{
    if (!armature.valid()) { return std::unexpected(error(ErrorCode::invalid_armature, "binding needs a valid armature")); }
    if (vertex_count > std::numeric_limits<u32>::max()) {
        return std::unexpected(error(ErrorCode::invalid_mesh, "binding vertex count exceeds the u32 limit"));
    }
    if (auto valid = rig::validate(options.mesh_to_armature); !valid) { return std::unexpected(valid.error()); }
    const auto to_armature = matrix(options.mesh_to_armature);
    auto from_armature = inverse_affine(to_armature);
    if (!from_armature) { return std::unexpected(from_armature.error()); }
    std::vector<Mat4> inverse_rest;
    inverse_rest.reserve(armature.bone_count());
    for (const auto& rest : armature.rest_globals()) {
        auto inverse = inverse_affine(rest);
        if (!inverse) { return std::unexpected(inverse.error()); }
        const auto result = multiply(*inverse, to_armature);
        if (auto check = inverse_affine(result); !check) { return std::unexpected(check.error()); }
        inverse_rest.push_back(result);
    }
    return SkinWeights{vertex_count, std::move(armature), options, *from_armature, std::move(inverse_rest)};
}

std::expected<std::span<const Influence>, Diagnostic> SkinWeights::influences(std::size_t vertex) const
{
    if (vertex >= vertex_count()) { return std::unexpected(error(ErrorCode::invalid_vertex, "vertex is outside the bound mesh snapshot", vertex)); }
    return std::span<const Influence>{influences_[vertex]};
}

std::expected<void, Diagnostic> SkinWeights::set_weights(std::size_t vertex, std::span<const Influence> influences)
{
    if (vertex >= vertex_count()) { return std::unexpected(error(ErrorCode::invalid_vertex, "vertex is outside the bound mesh snapshot", vertex)); }
    for (std::size_t i = 0; i < influences.size(); ++i) {
        const auto& value = influences[i];
        if (auto index = armature_.index(value.bone); !index) { return std::unexpected(error(ErrorCode::invalid_bone, index.error().message, vertex)); }
        if (!std::isfinite(value.weight) || value.weight < 0) {
            return std::unexpected(error(ErrorCode::invalid_weight, "weights must be finite and nonnegative", vertex));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (influences[j].bone == value.bone) {
                return std::unexpected(error(ErrorCode::invalid_weight, "each bone may occur only once in a vertex's weights", vertex));
            }
        }
    }
    // Copy first, including when the span points into this vertex's own values.
    std::vector<Influence> staged(influences.begin(), influences.end());
    influences_[vertex] = std::move(staged);
    ++revision_;
    return {};
}

std::expected<void, Diagnostic> SkinWeights::validate() const
{
    if (!armature_.valid() || inverse_rest_.size() != armature_.bone_count()) {
        return std::unexpected(error(ErrorCode::invalid_armature,
            "skin weights have no valid armature/bind snapshot (possibly moved-from)"));
    }
    for (std::size_t vertex = 0; vertex < vertex_count(); ++vertex) {
        double sum = 0;
        for (const auto& value : influences_[vertex]) { sum += value.weight; }
        if (!(sum > 0)) {
            return std::unexpected(error(ErrorCode::incomplete_weights, "every vertex needs a positive bone influence", vertex));
        }
        if (std::abs(sum - 1.0) > 1e-5) {
            return std::unexpected(error(ErrorCode::unnormalized_weights,
                "weights must sum to one; call normalize_weights() explicitly", vertex));
        }
    }
    return {};
}

std::expected<void, Diagnostic> SkinWeights::normalize_weights()
{
    if (!armature_.valid()) {
        return std::unexpected(error(ErrorCode::invalid_armature, "cannot normalize moved-from skin weights"));
    }
    auto staged = influences_;
    for (std::size_t vertex = 0; vertex < vertex_count(); ++vertex) {
        double sum = 0;
        for (const auto& value : staged[vertex]) { sum += value.weight; }
        if (!(sum > 0) || !std::isfinite(sum)) {
            return std::unexpected(error(ErrorCode::incomplete_weights, "cannot normalize a vertex without positive influence", vertex));
        }
        for (auto& value : staged[vertex]) { value.weight = static_cast<f32>(value.weight / sum); }
    }
    influences_ = std::move(staged);
    ++revision_;
    return {};
}

std::expected<PruningReport, Diagnostic> SkinWeights::prune_weights(u32 max_influences)
{
    if (max_influences == 0) { return std::unexpected(error(ErrorCode::invalid_influence_limit, "pruning must retain at least one influence")); }
    if (auto valid = validate(); !valid) { return std::unexpected(valid.error()); }
    auto staged = influences_;
    PruningReport report;
    for (std::size_t vertex = 0; vertex < vertex_count(); ++vertex) {
        auto& values = staged[vertex];
        std::erase_if(values, [](const auto& value) { return value.weight == 0; });
        if (values.size() <= max_influences) { continue; }
        std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
            if (left.weight != right.weight) { return left.weight > right.weight; }
            return left.bone.index_ < right.bone.index_;
        });
        double loss = 0;
        for (std::size_t i = max_influences; i < values.size(); ++i) { loss += values[i].weight; }
        report.vertices.push_back(VertexPruning{vertex, static_cast<u32>(values.size() - max_influences), static_cast<f32>(loss)});
        report.maximum_discarded_weight = std::max(report.maximum_discarded_weight, static_cast<f32>(loss));
        values.resize(max_influences);
        double retained = 0;
        for (const auto& value : values) { retained += value.weight; }
        for (auto& value : values) { value.weight = static_cast<f32>(value.weight / retained); }
    }
    influences_ = std::move(staged);
    ++revision_;
    return report;
}

std::expected<PackedInfluences, Diagnostic> SkinWeights::pack(u32 max_influences) const
{
    if (max_influences != 4 && max_influences != 8) {
        return std::unexpected(error(ErrorCode::invalid_influence_limit, "runtime packing supports exactly four or eight influences per vertex"));
    }
    if (auto valid = validate(); !valid) { return std::unexpected(valid.error()); }
    PackedInfluences result{max_influences, std::vector<PackedVertexInfluences>(vertex_count())};
    for (std::size_t vertex = 0; vertex < vertex_count(); ++vertex) {
        auto& packed = result.vertices[vertex];
        std::size_t slot = 0;
        for (const auto& value : influences_[vertex]) {
            if (value.weight == 0) { continue; }
            if (slot >= max_influences) {
                return std::unexpected(error(ErrorCode::too_many_influences,
                    "vertex exceeds the requested runtime influence count; prune explicitly or select eight influences", vertex));
            }
            auto& joints = slot < 4 ? packed.joints0 : packed.joints1;
            auto& weights = slot < 4 ? packed.weights0 : packed.weights1;
            joints[slot % 4] = value.bone.index_;
            weights[slot % 4] = value.weight;
            ++slot;
        }
    }
    return result;
}

std::expected<std::vector<Mat4>, Diagnostic> SkinWeights::palette(const Pose& pose) const
{
    if (!armature_.valid() || inverse_rest_.size() != armature_.bone_count()) {
        return std::unexpected(error(ErrorCode::invalid_armature,
            "cannot evaluate a palette without a valid armature/bind snapshot"));
    }
    if (pose.armature().identity() != armature_.identity()) {
        return std::unexpected(error(ErrorCode::incompatible_pose, "pose and skin binding must refer to the same immutable armature"));
    }
    auto globals = pose.globals();
    if (!globals) { return std::unexpected(globals.error()); }
    for (std::size_t bone = 0; bone < globals->size(); ++bone) {
        (*globals)[bone] = multiply(multiply(from_armature_, (*globals)[bone]), inverse_rest_[bone]);
        if (auto valid = inverse_affine((*globals)[bone]); !valid) { return std::unexpected(valid.error()); }
    }
    return globals;
}

std::expected<std::vector<Vec3>, Diagnostic> SkinWeights::deform_points(
    std::span<const Vec3> points, const Pose& pose) const
{
    if (points.size() != vertex_count()) { return std::unexpected(error(ErrorCode::invalid_mesh, "point count differs from the bound vertex domain")); }
    if (auto valid = validate(); !valid) { return std::unexpected(valid.error()); }
    auto matrices = palette(pose);
    if (!matrices) { return std::unexpected(matrices.error()); }
    std::vector<Vec3> result(vertex_count());
    for (std::size_t vertex = 0; vertex < vertex_count(); ++vertex) {
        if (!finite(points[vertex])) { return std::unexpected(error(ErrorCode::invalid_mesh, "point must be finite", vertex)); }
        double values[3]{};
        for (const auto& influence : influences_[vertex]) {
            if (influence.weight == 0) { continue; }
            const auto deformed = transform_point((*matrices)[influence.bone.index_], points[vertex]);
            for (std::size_t c = 0; c < 3; ++c) { values[c] += static_cast<double>(deformed[c]) * influence.weight; }
        }
        result[vertex] = {static_cast<f32>(values[0]), static_cast<f32>(values[1]), static_cast<f32>(values[2])};
        if (!finite(result[vertex])) { return std::unexpected(error(ErrorCode::invalid_matrix, "deformed point exceeds the finite f32 range", vertex)); }
    }
    return result;
}

std::expected<std::vector<Vec3>, Diagnostic> SkinWeights::deform_normals(
    std::span<const Vec3> normals, const Pose& pose) const
{
    if (normals.size() != vertex_count()) { return std::unexpected(error(ErrorCode::invalid_mesh, "normal count differs from the bound vertex domain")); }
    if (auto valid = validate(); !valid) { return std::unexpected(valid.error()); }
    auto matrices = palette(pose);
    if (!matrices) { return std::unexpected(matrices.error()); }
    for (auto& value : *matrices) {
        auto normal = normal_matrix(value);
        if (!normal) { return std::unexpected(normal.error()); }
        value = *normal;
    }
    std::vector<Vec3> result(vertex_count());
    for (std::size_t vertex = 0; vertex < vertex_count(); ++vertex) {
        if (!finite(normals[vertex])) { return std::unexpected(error(ErrorCode::invalid_mesh, "normal must be finite", vertex)); }
        double values[3]{};
        for (const auto& influence : influences_[vertex]) {
            if (influence.weight == 0) { continue; }
            const auto& transform = (*matrices)[influence.bone.index_];
            for (std::size_t row = 0; row < 3; ++row) {
                double component = 0;
                for (std::size_t column = 0; column < 3; ++column) {
                    component += static_cast<double>(transform[column][row]) * normals[vertex][column];
                }
                values[row] += component * influence.weight;
            }
        }
        const double length = std::sqrt(values[0]*values[0] + values[1]*values[1] + values[2]*values[2]);
        // Every finite, nonzero f32 direction is normalizable here: accumulation
        // and the length calculation use double, so no arbitrary magnitude
        // threshold is needed for valid tiny directions or large uniform scales.
        if (!std::isfinite(length) || !(length > 0)) {
            return std::unexpected(error(ErrorCode::invalid_matrix, "skinning produced a degenerate normal", vertex));
        }
        result[vertex] = {static_cast<f32>(values[0]/length), static_cast<f32>(values[1]/length), static_cast<f32>(values[2]/length)};
    }
    return result;
}

} // namespace vng::rig
