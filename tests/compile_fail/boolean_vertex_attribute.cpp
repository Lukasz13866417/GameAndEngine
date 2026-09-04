#include <vng/gfx/gfx.hpp>

#include <cstddef>

struct Flag : vng::gfx::Semantic<bool> {};

struct BoolCodec {
    using value_type = bool;
    using logical_type = value_type;

    static constexpr std::size_t size = 1;
    static constexpr std::size_t alignment = 1;
    static constexpr vng::gfx::VertexFormat format{
        vng::gfx::ComponentEncoding::Unsigned8,
        vng::gfx::AttributeInterpretation::Integer,
        1,
        1,
        1,
        1,
    };

    static void encode(std::byte*, const value_type&) noexcept {}
    [[nodiscard]] static value_type decode(const std::byte*) noexcept { return {}; }
};

using InvalidRecord = vng::gfx::Record<vng::gfx::as<Flag, BoolCodec>>;
InvalidRecord force_instantiation;
