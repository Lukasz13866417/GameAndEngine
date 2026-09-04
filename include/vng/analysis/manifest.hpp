#pragma once

#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <vng/analysis/types.hpp>
#include <vng/gfx/mesh.hpp>

namespace vng::analysis {

// A draw-local primitive resolved back to the source mesh topology. Retaining
// the three source vertices makes this useful even after the source CPU mesh
// has been released.
struct PrimitiveSource final {
    u32 face{};
    gfx::TriangleFace vertices{};

    friend constexpr bool operator==(const PrimitiveSource&,
                                     const PrimitiveSource&) = default;
};

class PrimitiveSourceMap final {
public:
    PrimitiveSourceMap() = default;

    explicit PrimitiveSourceMap(std::vector<PrimitiveSource> sources)
        : sources_(std::move(sources))
    {}

    [[nodiscard]] std::size_t size() const noexcept { return sources_.size(); }
    [[nodiscard]] bool empty() const noexcept { return sources_.empty(); }

    [[nodiscard]] const PrimitiveSource* find(PrimitiveId primitive) const noexcept
    {
        const auto index = static_cast<std::size_t>(primitive.value);
        return index < sources_.size() ? &sources_[index] : nullptr;
    }

    [[nodiscard]] std::span<const PrimitiveSource> sources() const noexcept
    {
        return sources_;
    }

private:
    std::vector<PrimitiveSource> sources_;
};

using SharedPrimitiveSourceMap = std::shared_ptr<const PrimitiveSourceMap>;

struct FaceRange final {
    u32 first{};
    u32 count{};

    friend constexpr bool operator==(const FaceRange&, const FaceRange&) = default;
};

namespace detail {

template<class... Records>
[[nodiscard]] inline SharedPrimitiveSourceMap make_primitive_source_map(
    const gfx::Mesh<Records...>& mesh,
    std::size_t first,
    std::size_t count)
{
    if (first > mesh.face_count() || count > mesh.face_count() - first) {
        throw std::out_of_range(
            "analysis primitive source range is outside the mesh face list");
    }
    if (first + count
        > static_cast<std::size_t>(std::numeric_limits<u32>::max())) {
        throw std::length_error(
            "analysis primitive source faces exceed the u32 source-index range");
    }

    std::vector<PrimitiveSource> sources;
    sources.reserve(count);
    for (std::size_t local = 0; local < count; ++local) {
        const auto source_face = first + local;
        sources.push_back(PrimitiveSource{
            .face = static_cast<u32>(source_face),
            .vertices = mesh.faces()[source_face],
        });
    }
    return std::make_shared<const PrimitiveSourceMap>(std::move(sources));
}

} // namespace detail

// The ordinary whole-mesh path. Primitive N produced by GpuMesh currently
// maps to source face N, and this function records that fact automatically.
template<class... Records>
[[nodiscard]] SharedPrimitiveSourceMap primitive_sources(
    const gfx::Mesh<Records...>& mesh)
{
    return detail::make_primitive_source_map(mesh, 0, mesh.face_count());
}

// A range overload supports submesh draws without changing the pixel format.
// Primitive zero maps to the first face in the requested range.
template<class... Records>
[[nodiscard]] SharedPrimitiveSourceMap primitive_sources(
    const gfx::Mesh<Records...>& mesh,
    FaceRange range)
{
    return detail::make_primitive_source_map(
        mesh,
        static_cast<std::size_t>(range.first),
        static_cast<std::size_t>(range.count));
}

struct RenderItemProvenance final {
    std::optional<EntityId> entity;
    MeshAssetId mesh{};
    AssetRevision mesh_revision{};
    std::optional<MaterialId> material;
    Mat4 object_to_world{Mat4::identity()};

    friend constexpr bool operator==(const RenderItemProvenance&,
                                     const RenderItemProvenance&) = default;
};

struct ManifestItem final {
    FrameItemId id{};
    RenderItemProvenance provenance;
    SharedPrimitiveSourceMap primitives;
};

struct ManifestSurface final {
    const ManifestItem* item{};
    const PrimitiveSource* primitive{};
};

class AnalysisManifest final {
public:
    explicit AnalysisManifest(FrameId frame = {}) noexcept
        : frame_(frame)
    {}

    [[nodiscard]] FrameId frame() const noexcept { return frame_; }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

    void reserve(std::size_t count)
    {
        if (count > static_cast<std::size_t>(
                        std::numeric_limits<u32>::max())) {
            throw std::length_error(
                "analysis manifest cannot contain more than u32 frame items");
        }
        items_.reserve(count);
    }

    // IDs are dense and automatically assigned. Callers never place stable
    // entity or asset IDs directly in the per-pixel surface-key image.
    [[nodiscard]] FrameItemId add(
        RenderItemProvenance provenance,
        SharedPrimitiveSourceMap primitives)
    {
        if (!primitives) {
            throw std::invalid_argument(
                "analysis manifest item requires a primitive source map");
        }
        if (items_.size()
            >= static_cast<std::size_t>(std::numeric_limits<u32>::max())) {
            throw std::length_error(
                "analysis manifest exhausted its frame-item ID space");
        }

        const auto id = FrameItemId{static_cast<u32>(items_.size() + 1)};
        items_.push_back(ManifestItem{
            .id = id,
            .provenance = std::move(provenance),
            .primitives = std::move(primitives),
        });
        return id;
    }

    [[nodiscard]] const ManifestItem* find(FrameItemId id) const noexcept
    {
        if (!id) {
            return nullptr;
        }
        const auto index = static_cast<std::size_t>(id.value - 1);
        return index < items_.size() ? &items_[index] : nullptr;
    }

    [[nodiscard]] std::optional<ManifestSurface> resolve(
        SurfaceKey key) const noexcept
    {
        const auto* item = find(key.item);
        if (!item) {
            return std::nullopt;
        }
        const auto* primitive = item->primitives->find(key.primitive);
        if (!primitive) {
            return std::nullopt;
        }
        return ManifestSurface{item, primitive};
    }

    [[nodiscard]] std::span<const ManifestItem> items() const noexcept
    {
        return items_;
    }

private:
    FrameId frame_{};
    std::vector<ManifestItem> items_;
};

} // namespace vng::analysis
