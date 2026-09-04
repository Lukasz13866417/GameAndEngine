#include <vng/gfx/gfx.hpp>

#include <cstddef>

struct Transform : vng::gfx::Semantic<vng::Mat4> {};

struct MatrixCodec {
    using value_type = vng::Mat4;
    using logical_type = value_type;

    static constexpr std::size_t size = 16;
    static constexpr std::size_t alignment = 4;
    static constexpr vng::gfx::VertexFormat format{
        vng::gfx::ComponentEncoding::Float32,
        vng::gfx::AttributeInterpretation::FloatingPoint,
        4,
        4,
        16,
        4,
    };

    static void encode(std::byte*, const value_type&) noexcept {}
    [[nodiscard]] static value_type decode(const std::byte*) noexcept { return {}; }
};

using InvalidRecord = vng::gfx::Record<vng::gfx::as<Transform, MatrixCodec>>;
InvalidRecord force_instantiation;
