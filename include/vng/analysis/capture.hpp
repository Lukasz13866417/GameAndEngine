#pragma once

#include <cmath>
#include <expected>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <vng/analysis/image.hpp>
#include <vng/analysis/manifest.hpp>

namespace vng::analysis {

enum class CaptureDiagnosticCode {
    invalid_frame,
    mismatched_extent,
    noncanonical_background_key,
    unknown_surface_key,
    pixel_out_of_bounds,
};

struct CaptureDiagnostic final {
    CaptureDiagnosticCode code{};
    std::string message;
    std::optional<Channel> channel;
    std::optional<Extent2D> expected_extent;
    std::optional<Extent2D> actual_extent;
    std::optional<Pixel> pixel;
    std::optional<SurfaceKey> surface_key;

    friend bool operator==(const CaptureDiagnostic&,
                           const CaptureDiagnostic&) = default;
};

struct SurfaceHit final {
    FrameId frame{};
    Pixel pixel{};
    SurfaceKey key{};
    Rgba8 color{};
    f32 device_depth{};

    std::optional<EntityId> entity;
    MeshAssetId mesh{};
    AssetRevision mesh_revision{};
    std::optional<MaterialId> material;
    Mat4 object_to_world{Mat4::identity()};

    u32 face{};
    gfx::TriangleFace vertices{};

    friend constexpr bool operator==(const SurfaceHit&, const SurfaceHit&) = default;
};

// Inclusive row-major pixel bounds. They are optional in summaries because an
// item that is present in the manifest need not survive visibility testing.
struct PixelBounds final {
    Pixel minimum{};
    Pixel maximum{};

    [[nodiscard]] constexpr u32 width() const noexcept
    {
        return maximum.x - minimum.x + 1;
    }

    [[nodiscard]] constexpr u32 height() const noexcept
    {
        return maximum.y - minimum.y + 1;
    }

    [[nodiscard]] constexpr bool contains(Pixel pixel) const noexcept
    {
        return pixel.x >= minimum.x && pixel.x <= maximum.x
            && pixel.y >= minimum.y && pixel.y <= maximum.y;
    }

    friend constexpr bool operator==(const PixelBounds&,
                                     const PixelBounds&) = default;
};

struct DepthSummary final {
    f32 minimum{};
    f32 maximum{};
    Pixel minimum_pixel{};
    Pixel maximum_pixel{};
    u64 finite_pixel_count{};

    friend constexpr bool operator==(const DepthSummary&,
                                     const DepthSummary&) = default;
};

struct ItemCaptureSummary final {
    FrameItemId item{};
    u64 covered_pixel_count{};
    u64 visible_primitive_count{};
    u64 non_finite_depth_count{};
    std::optional<PixelBounds> bounds;
    std::optional<DepthSummary> device_depth;

    [[nodiscard]] constexpr bool visible() const noexcept
    {
        return covered_pixel_count != 0;
    }

    friend constexpr bool operator==(const ItemCaptureSummary&,
                                     const ItemCaptureSummary&) = default;
};

struct CaptureSummary final {
    Extent2D extent{};
    u64 pixel_count{};
    u64 covered_pixel_count{};
    u64 background_pixel_count{};
    u64 visible_item_count{};
    u64 visible_primitive_count{};
    u64 non_finite_depth_count{};
    std::optional<PixelBounds> covered_bounds;
    std::optional<DepthSummary> device_depth;
    std::vector<ItemCaptureSummary> items;

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return pixel_count == 0;
    }

    friend bool operator==(const CaptureSummary&,
                           const CaptureSummary&) = default;
};

// Unlike surface_at(), inspect() distinguishes an invalid coordinate from a
// valid background pixel and always reports the raw color/depth/key triplet.
// Later evidence channels can be layered on this value without changing the
// established SurfaceHit API.
struct PixelInspection final {
    FrameId frame{};
    Pixel pixel{};
    Rgba8 color{};
    f32 device_depth{};
    SurfaceKey surface_key{};
    std::optional<SurfaceHit> surface;

    [[nodiscard]] constexpr bool has_surface() const noexcept
    {
        return surface.has_value();
    }

    [[nodiscard]] constexpr bool background() const noexcept
    {
        return !has_surface();
    }

    friend constexpr bool operator==(const PixelInspection&,
                                     const PixelInspection&) = default;
};

class AnalysisCapture final {
public:
    using ColorImage = Image<channel_pixel_t<Channel::color>>;
    using DepthImage = Image<channel_pixel_t<Channel::device_depth>>;
    using SurfaceImage = Image<channel_pixel_t<Channel::surface_key>>;

    [[nodiscard]] static std::expected<AnalysisCapture, CaptureDiagnostic> create(
        ColorImage color,
        DepthImage device_depth,
        SurfaceImage surface_keys,
        AnalysisManifest manifest)
    {
        if (!manifest.frame()) {
            return std::unexpected(CaptureDiagnostic{
                .code = CaptureDiagnosticCode::invalid_frame,
                .message = "analysis capture requires a nonzero frame ID",
                .channel = std::nullopt,
                .expected_extent = std::nullopt,
                .actual_extent = std::nullopt,
                .pixel = std::nullopt,
                .surface_key = std::nullopt,
            });
        }

        const auto extent = color.extent();
        if (device_depth.extent() != extent) {
            return std::unexpected(extent_diagnostic(
                Channel::device_depth, extent, device_depth.extent()));
        }
        if (surface_keys.extent() != extent) {
            return std::unexpected(extent_diagnostic(
                Channel::surface_key, extent, surface_keys.extent()));
        }

        // Validate once at the CPU boundary. Callers can then use the compact
        // optional-returning surface_at() API without handling corrupt-key
        // errors on every lookup.
        for (u32 y = 0; y < extent.height; ++y) {
            for (u32 x = 0; x < extent.width; ++x) {
                const Pixel pixel{x, y};
                const auto key = surface_keys.at(pixel);
                if (!key.has_surface()) {
                    if (key != SurfaceKey::background()) {
                        return std::unexpected(CaptureDiagnostic{
                            .code = CaptureDiagnosticCode::noncanonical_background_key,
                            .message = "analysis capture contains a background surface key with a non-invalid primitive",
                            .channel = Channel::surface_key,
                            .expected_extent = std::nullopt,
                            .actual_extent = std::nullopt,
                            .pixel = pixel,
                            .surface_key = key,
                        });
                    }
                    continue;
                }
                if (!manifest.resolve(key)) {
                    return std::unexpected(CaptureDiagnostic{
                        .code = CaptureDiagnosticCode::unknown_surface_key,
                        .message = "analysis capture surface key is absent from the frame manifest",
                        .channel = Channel::surface_key,
                        .expected_extent = std::nullopt,
                        .actual_extent = std::nullopt,
                        .pixel = pixel,
                        .surface_key = key,
                    });
                }
            }
        }

        auto summary = summarize(
            color, device_depth, surface_keys, manifest);

        return AnalysisCapture{
            std::move(color),
            std::move(device_depth),
            std::move(surface_keys),
            std::move(manifest),
            std::move(summary),
        };
    }

    [[nodiscard]] Extent2D extent() const noexcept { return color_.extent(); }
    [[nodiscard]] FrameId frame() const noexcept { return manifest_.frame(); }

    [[nodiscard]] const ColorImage& color() const noexcept { return color_; }
    [[nodiscard]] const DepthImage& device_depth() const noexcept
    {
        return device_depth_;
    }
    [[nodiscard]] const SurfaceImage& surface_keys() const noexcept
    {
        return surface_keys_;
    }
    [[nodiscard]] const AnalysisManifest& manifest() const noexcept
    {
        return manifest_;
    }
    [[nodiscard]] const CaptureSummary& summary() const noexcept
    {
        return summary_;
    }

    template<Channel C>
    [[nodiscard]] const Image<channel_pixel_t<C>>& channel() const noexcept
    {
        if constexpr (C == Channel::color) {
            return color_;
        } else if constexpr (C == Channel::device_depth) {
            return device_depth_;
        } else {
            static_assert(C == Channel::surface_key);
            return surface_keys_;
        }
    }

    // Out-of-bounds and background pixels both have no surface. All nonzero
    // keys are guaranteed to resolve because create() validates the capture.
    [[nodiscard]] std::optional<SurfaceHit> surface_at(Pixel pixel) const
    {
        if (!surface_keys_.contains(pixel)) {
            return std::nullopt;
        }

        const auto key = surface_keys_.at(pixel);
        const auto resolved = manifest_.resolve(key);
        if (!resolved) {
            return std::nullopt;
        }

        const auto& provenance = resolved->item->provenance;
        return SurfaceHit{
            .frame = manifest_.frame(),
            .pixel = pixel,
            .key = key,
            .color = color_.at(pixel),
            .device_depth = device_depth_.at(pixel),
            .entity = provenance.entity,
            .mesh = provenance.mesh,
            .mesh_revision = provenance.mesh_revision,
            .material = provenance.material,
            .object_to_world = provenance.object_to_world,
            .face = resolved->primitive->face,
            .vertices = resolved->primitive->vertices,
        };
    }

    [[nodiscard]] std::expected<PixelInspection, CaptureDiagnostic> inspect(
        Pixel pixel) const
    {
        if (!color_.contains(pixel)) {
            return std::unexpected(CaptureDiagnostic{
                .code = CaptureDiagnosticCode::pixel_out_of_bounds,
                .message = "analysis inspection pixel is outside the capture",
                .channel = std::nullopt,
                .expected_extent = color_.extent(),
                .actual_extent = std::nullopt,
                .pixel = pixel,
                .surface_key = std::nullopt,
            });
        }

        return PixelInspection{
            .frame = manifest_.frame(),
            .pixel = pixel,
            .color = color_.at(pixel),
            .device_depth = device_depth_.at(pixel),
            .surface_key = surface_keys_.at(pixel),
            .surface = surface_at(pixel),
        };
    }

private:
    AnalysisCapture(
        ColorImage color,
        DepthImage device_depth,
        SurfaceImage surface_keys,
        AnalysisManifest manifest,
        CaptureSummary summary) noexcept
        : color_(std::move(color)),
          device_depth_(std::move(device_depth)),
          surface_keys_(std::move(surface_keys)),
          manifest_(std::move(manifest)),
          summary_(std::move(summary))
    {}

    static void include_pixel(
        std::optional<PixelBounds>& bounds,
        Pixel pixel) noexcept
    {
        if (!bounds) {
            bounds = PixelBounds{pixel, pixel};
            return;
        }
        bounds->minimum.x = pixel.x < bounds->minimum.x
            ? pixel.x : bounds->minimum.x;
        bounds->minimum.y = pixel.y < bounds->minimum.y
            ? pixel.y : bounds->minimum.y;
        bounds->maximum.x = pixel.x > bounds->maximum.x
            ? pixel.x : bounds->maximum.x;
        bounds->maximum.y = pixel.y > bounds->maximum.y
            ? pixel.y : bounds->maximum.y;
    }

    static void include_depth(
        std::optional<DepthSummary>& summary,
        f32 depth,
        Pixel pixel) noexcept
    {
        if (!summary) {
            summary = DepthSummary{
                .minimum = depth,
                .maximum = depth,
                .minimum_pixel = pixel,
                .maximum_pixel = pixel,
                .finite_pixel_count = 1,
            };
            return;
        }
        ++summary->finite_pixel_count;
        if (depth < summary->minimum) {
            summary->minimum = depth;
            summary->minimum_pixel = pixel;
        }
        if (depth > summary->maximum) {
            summary->maximum = depth;
            summary->maximum_pixel = pixel;
        }
    }

    [[nodiscard]] static CaptureSummary summarize(
        const ColorImage& color,
        const DepthImage& device_depth,
        const SurfaceImage& surface_keys,
        const AnalysisManifest& manifest)
    {
        CaptureSummary result;
        result.extent = color.extent();
        result.pixel_count = static_cast<u64>(color.size());
        result.items.reserve(manifest.size());
        for (const auto& item : manifest.items()) {
            ItemCaptureSummary item_summary;
            item_summary.item = item.id;
            result.items.push_back(std::move(item_summary));
        }

        std::set<std::pair<u32, u32>> visible_primitives;
        for (u32 y = 0; y < color.height(); ++y) {
            for (u32 x = 0; x < color.width(); ++x) {
                const Pixel pixel{x, y};
                const auto key = surface_keys.at(pixel);
                if (!key.has_surface()) {
                    ++result.background_pixel_count;
                    continue;
                }

                ++result.covered_pixel_count;
                include_pixel(result.covered_bounds, pixel);

                auto& item = result.items[static_cast<std::size_t>(
                    key.item.value - 1)];
                if (item.covered_pixel_count == 0) {
                    ++result.visible_item_count;
                }
                ++item.covered_pixel_count;
                include_pixel(item.bounds, pixel);

                if (visible_primitives.emplace(
                        key.item.value, key.primitive.value).second) {
                    ++result.visible_primitive_count;
                    ++item.visible_primitive_count;
                }

                const auto depth = device_depth.at(pixel);
                if (std::isfinite(depth)) {
                    include_depth(result.device_depth, depth, pixel);
                    include_depth(item.device_depth, depth, pixel);
                } else {
                    ++result.non_finite_depth_count;
                    ++item.non_finite_depth_count;
                }
            }
        }
        return result;
    }

    [[nodiscard]] static CaptureDiagnostic extent_diagnostic(
        Channel channel,
        Extent2D expected,
        Extent2D actual)
    {
        return CaptureDiagnostic{
            .code = CaptureDiagnosticCode::mismatched_extent,
            .message = "analysis capture channels must have identical extents",
            .channel = channel,
            .expected_extent = expected,
            .actual_extent = actual,
            .pixel = std::nullopt,
            .surface_key = std::nullopt,
        };
    }

    ColorImage color_;
    DepthImage device_depth_;
    SurfaceImage surface_keys_;
    AnalysisManifest manifest_;
    CaptureSummary summary_;
};

} // namespace vng::analysis
