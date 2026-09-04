#include <vng/gfx/gfx.hpp>

#include <cstddef>

struct Color : vng::gfx::Semantic<vng::Vec4> {};

struct WrongShapeCodec {
    using value_type = vng::Vec4;
    using logical_type = value_type;

    static constexpr std::size_t size = 2;
    static constexpr std::size_t alignment = 1;
    static constexpr vng::gfx::VertexFormat format{
        vng::gfx::ComponentEncoding::Unsigned8,
        vng::gfx::AttributeInterpretation::Normalized,
        2,
        2,
        2,
        1,
    };

    static void encode(std::byte*, const value_type&) noexcept {}
    [[nodiscard]] static value_type decode(const std::byte*) noexcept { return {}; }
};

using InvalidRecord = vng::gfx::Record<vng::gfx::as<Color, WrongShapeCodec>>;
InvalidRecord force_instantiation;
