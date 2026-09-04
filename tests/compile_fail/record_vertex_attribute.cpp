#include <vng/gfx/gfx.hpp>

#include <cstddef>

struct Position : vng::gfx::Semantic<vng::Vec2> {};
using Nested = vng::gfx::Record<Position>;
struct NestedValue : vng::gfx::Semantic<Nested> {};

struct RecordCodec {
    using value_type = Nested;
    using logical_type = value_type;

    static constexpr std::size_t size = 8;
    static constexpr std::size_t alignment = 4;
    static constexpr vng::gfx::VertexFormat format{
        vng::gfx::ComponentEncoding::Float32,
        vng::gfx::AttributeInterpretation::FloatingPoint,
        2,
        2,
        8,
        4,
    };

    static void encode(std::byte*, const value_type&) noexcept {}
    [[nodiscard]] static value_type decode(const std::byte*) noexcept { return {}; }
};

using InvalidRecord = vng::gfx::Record<vng::gfx::as<NestedValue, RecordCodec>>;
InvalidRecord force_instantiation;
