#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <limits>
#include <type_traits>

#include <vng/core/types.hpp>

namespace vng::analysis {

template<class Tag, std::unsigned_integral Representation>
struct Id final {
    using representation_type = Representation;

    Representation value{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return value != Representation{};
    }

    friend constexpr bool operator==(Id, Id) = default;
    friend constexpr auto operator<=>(Id, Id) = default;
};

// FrameItemId is intentionally local to one capture. It is a compact lookup
// key, not a persistent entity or asset identifier. Zero always means that no
// rendered item owns the pixel.
using FrameId = Id<struct FrameIdTag, u64>;
using FrameItemId = Id<struct FrameItemIdTag, u32>;
using PrimitiveId = Id<struct PrimitiveIdTag, u32>;

using EntityId = Id<struct EntityIdTag, u64>;
using MeshAssetId = Id<struct MeshAssetIdTag, u64>;
using MaterialId = Id<struct MaterialIdTag, u64>;
using AssetRevision = Id<struct AssetRevisionTag, u64>;

inline constexpr FrameItemId background_item{};
inline constexpr PrimitiveId invalid_primitive{
    std::numeric_limits<u32>::max()};

// This is the exact, backend-neutral representation stored in the analysis
// surface-key image. Keep rich provenance in AnalysisManifest rather than
// expanding this per-pixel value.
struct SurfaceKey final {
    FrameItemId item{};
    PrimitiveId primitive{invalid_primitive};

    [[nodiscard]] constexpr bool has_surface() const noexcept
    {
        return static_cast<bool>(item);
    }

    [[nodiscard]] static constexpr SurfaceKey background() noexcept
    {
        return {};
    }

    [[nodiscard]] constexpr UVec2 raw() const noexcept
    {
        return {item.value, primitive.value};
    }

    [[nodiscard]] static constexpr SurfaceKey from_raw(UVec2 raw) noexcept
    {
        return {FrameItemId{raw.x}, PrimitiveId{raw.y}};
    }

    friend constexpr bool operator==(const SurfaceKey&, const SurfaceKey&) = default;
};

enum class Channel {
    color,
    device_depth,
    surface_key,
};

struct Rgba8 final {
    u8 r{};
    u8 g{};
    u8 b{};
    u8 a{};

    friend constexpr bool operator==(const Rgba8&, const Rgba8&) = default;
};

using Extent2D = ::vng::Extent2D;

struct Pixel final {
    u32 x{};
    u32 y{};

    friend constexpr bool operator==(const Pixel&, const Pixel&) = default;
};

template<Channel>
struct ChannelTraits;

template<>
struct ChannelTraits<Channel::color> {
    using pixel_type = Rgba8;
};

template<>
struct ChannelTraits<Channel::device_depth> {
    using pixel_type = f32;
};

template<>
struct ChannelTraits<Channel::surface_key> {
    using pixel_type = SurfaceKey;
};

template<Channel C>
using channel_pixel_t = typename ChannelTraits<C>::pixel_type;

static_assert(!std::convertible_to<FrameItemId, u32>);
static_assert(!std::convertible_to<EntityId, MeshAssetId>);
static_assert(std::is_standard_layout_v<SurfaceKey>);
static_assert(std::is_trivially_copyable_v<SurfaceKey>);
static_assert(sizeof(SurfaceKey) == sizeof(UVec2));
static_assert(offsetof(SurfaceKey, item) == 0);
static_assert(offsetof(SurfaceKey, primitive) == sizeof(u32));
static_assert(sizeof(Rgba8) == 4);

} // namespace vng::analysis
