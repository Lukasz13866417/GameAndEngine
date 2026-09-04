#include <vng/gfx/gfx.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec2> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};
struct Normal : vng::gfx::Semantic<vng::Vec3> {};
struct InstanceOffset : vng::gfx::Semantic<vng::Vec2> {};
struct ObjectId : vng::gfx::Semantic<vng::u32> {};
struct Missing : vng::gfx::Semantic<vng::f32> {};

using PackedVertex = vng::gfx::Record<
    vng::gfx::as<Color, vng::gfx::unorm8x4>,
    Position,
    vng::gfx::as<Normal, vng::gfx::snorm10x3>,
    ObjectId>;

using Instance = vng::gfx::Record<InstanceOffset>;
using Layout = vng::gfx::VertexLayout<
    vng::gfx::Stream<PackedVertex>,
    vng::gfx::Stream<Instance, vng::gfx::PerInstance<1>>>;

struct ShaderInputs {
    using semantics = vng::gfx::TypeList<Position, Color, InstanceOffset, ObjectId>;
};

[[nodiscard]] bool close(float left, float right, float tolerance)
{
    return std::abs(left - right) <= tolerance;
}

template<class Codec>
[[nodiscard]] typename Codec::value_type round_trip(
    const typename Codec::value_type& value)
{
    std::array<std::byte, Codec::size> encoded{};
    Codec::encode(encoded.data(), value);
    return Codec::decode(encoded.data());
}

} // namespace

static_assert(PackedVertex::has(Position{}));
static_assert(PackedVertex::has(Color{}));
static_assert(!PackedVertex::has(Missing{}));
static_assert(PackedVertex::offset(Color{}) == 0);
static_assert(PackedVertex::offset(Position{}) == 4);
static_assert(PackedVertex::offset(Normal{}) == 12);
static_assert(PackedVertex::offset(ObjectId{}) == 16);
static_assert(PackedVertex::stride == 20);
static_assert(PackedVertex::alignment == 4);
static_assert(std::same_as<vng::gfx::record_value_t<PackedVertex, Normal>, vng::Vec3>);
static_assert(std::same_as<vng::gfx::record_codec_t<PackedVertex, Color>, vng::gfx::unorm8x4>);
static_assert(std::is_trivially_copyable_v<PackedVertex>);
static_assert(sizeof(PackedVertex) == PackedVertex::stride);
static_assert(vng::gfx::VertexCodec<vng::gfx::f32x1>);
static_assert(vng::gfx::VertexCodec<vng::gfx::f32x2>);
static_assert(vng::gfx::VertexCodec<vng::gfx::f32x3>);
static_assert(vng::gfx::VertexCodec<vng::gfx::f32x4>);
static_assert(vng::gfx::VertexCodec<vng::gfx::i32x1>);
static_assert(vng::gfx::VertexCodec<vng::gfx::i32x2>);
static_assert(vng::gfx::VertexCodec<vng::gfx::i32x3>);
static_assert(vng::gfx::VertexCodec<vng::gfx::i32x4>);
static_assert(vng::gfx::VertexCodec<vng::gfx::u32x1>);
static_assert(vng::gfx::VertexCodec<vng::gfx::u32x2>);
static_assert(vng::gfx::VertexCodec<vng::gfx::u32x3>);
static_assert(vng::gfx::VertexCodec<vng::gfx::u32x4>);
static_assert(vng::gfx::VertexCodec<vng::gfx::f16x2>);
static_assert(vng::gfx::VertexCodec<vng::gfx::f16x4>);
static_assert(vng::gfx::VertexCodec<vng::gfx::unorm8x2>);
static_assert(vng::gfx::VertexCodec<vng::gfx::unorm8x4>);
static_assert(vng::gfx::VertexCodec<vng::gfx::snorm8x2>);
static_assert(vng::gfx::VertexCodec<vng::gfx::snorm8x4>);
static_assert(vng::gfx::VertexCodec<vng::gfx::snorm10x3>);
static_assert(vng::gfx::valid_vertex_format(vng::gfx::f32x3::format));
static_assert(vng::gfx::valid_vertex_format(vng::gfx::snorm10x3::format));
static_assert(vng::gfx::vertex_format_compatible<vng::Vec4>(vng::gfx::unorm8x4::format));
static_assert(vng::gfx::vertex_format_compatible<vng::IVec3>(vng::gfx::i32x3::format));
static_assert(vng::gfx::vertex_format_compatible<vng::UVec2>(vng::gfx::u32x2::format));
static_assert(!vng::gfx::vertex_format_compatible<bool>(vng::gfx::u32x1::format));
static_assert(!vng::gfx::vertex_format_compatible<vng::Mat4>(vng::gfx::f32x4::format));
static_assert(!vng::gfx::vertex_format_compatible<PackedVertex>(vng::gfx::f32x4::format));

static_assert(vng::gfx::layout_satisfies_v<Layout, ShaderInputs>);
static_assert(vng::gfx::layout_satisfies_v<Layout, vng::gfx::TypeList<Color, Position>>);
static_assert(!vng::gfx::layout_satisfies_v<Layout, vng::gfx::TypeList<Missing>>);
static_assert(vng::gfx::layout_attribute<Layout, Position>::binding == 0);
static_assert(vng::gfx::layout_attribute<Layout, InstanceOffset>::binding == 1);
static_assert(vng::gfx::layout_attribute<Layout, InstanceOffset>::divisor == 1);

TEST_CASE("records use explicit deterministic packing and field codecs", "[gfx][record]")
{
    PackedVertex vertex;
    vertex.set(Color{}, {1.0F, 0.5F, -2.0F, 2.0F});
    vertex.set(Position{}, {-0.5F, 0.25F});
    vertex.set(Normal{}, {-1.0F, 0.25F, 1.0F});
    vertex.set(ObjectId{}, 0xFEED'1234U);

    const auto color = vertex.get(Color{});
    CHECK(close(color.x, 1.0F, 1.0F / 255.0F));
    CHECK(close(color.y, 128.0F / 255.0F, 1.0F / 255.0F));
    CHECK(color.z == 0.0F);
    CHECK(color.w == 1.0F);
    CHECK(vertex.get(Position{}) == vng::Vec2{-0.5F, 0.25F});
    CHECK(vertex.get(ObjectId{}) == 0xFEED'1234U);

    const auto encoded = vertex.bytes();
    CHECK(std::to_integer<unsigned>(encoded[0]) == 255U);
    CHECK(std::to_integer<unsigned>(encoded[1]) == 128U);
    CHECK(std::to_integer<unsigned>(encoded[2]) == 0U);
    CHECK(std::to_integer<unsigned>(encoded[3]) == 255U);

    const auto normal = vertex.get(Normal{});
    CHECK(close(normal.x, -1.0F, 1.0F / 511.0F));
    CHECK(close(normal.y, 0.25F, 1.0F / 511.0F));
    CHECK(close(normal.z, 1.0F, 1.0F / 511.0F));

    std::size_t visited = 0;
    PackedVertex::for_each_field([&](auto field) {
        CHECK(field.offset < PackedVertex::stride);
        ++visited;
    });
    CHECK(visited == PackedVertex::field_count);
}

TEST_CASE("raw scalar and vector codecs round trip exactly", "[gfx][codec]")
{
    CHECK(round_trip<vng::gfx::f32x1>(-123.25F) == -123.25F);
    CHECK(round_trip<vng::gfx::f32x2>({-1.0F, 8.5F}) == vng::Vec2{-1.0F, 8.5F});
    CHECK(round_trip<vng::gfx::f32x3>({-1.0F, 0.0F, 8.5F}) == vng::Vec3{-1.0F, 0.0F, 8.5F});
    CHECK(round_trip<vng::gfx::f32x4>({-1.0F, 0.0F, 8.5F, 42.0F})
          == vng::Vec4{-1.0F, 0.0F, 8.5F, 42.0F});

    CHECK(round_trip<vng::gfx::i32x1>(std::numeric_limits<vng::i32>::min())
          == std::numeric_limits<vng::i32>::min());
    CHECK(round_trip<vng::gfx::i32x2>({-1, std::numeric_limits<vng::i32>::max()})
          == vng::IVec2{-1, std::numeric_limits<vng::i32>::max()});
    CHECK(round_trip<vng::gfx::i32x3>({-1, 0, 1}) == vng::IVec3{-1, 0, 1});
    CHECK(round_trip<vng::gfx::i32x4>({-2, -1, 0, 1}) == vng::IVec4{-2, -1, 0, 1});

    CHECK(round_trip<vng::gfx::u32x1>(std::numeric_limits<vng::u32>::max())
          == std::numeric_limits<vng::u32>::max());
    CHECK(round_trip<vng::gfx::u32x2>({0U, std::numeric_limits<vng::u32>::max()})
          == vng::UVec2{0U, std::numeric_limits<vng::u32>::max()});
    CHECK(round_trip<vng::gfx::u32x3>({0U, 1U, 2U}) == vng::UVec3{0U, 1U, 2U});
    CHECK(round_trip<vng::gfx::u32x4>({0U, 1U, 2U, 3U}) == vng::UVec4{0U, 1U, 2U, 3U});
}

TEST_CASE("half codecs preserve representable extrema and special values", "[gfx][codec]")
{
    const auto pair = round_trip<vng::gfx::f16x2>({65504.0F, 0.00006103515625F});
    CHECK(pair == vng::Vec2{65504.0F, 0.00006103515625F});

    const auto values = round_trip<vng::gfx::f16x4>({0.0F, -0.0F, 1.0F, -2.0F});
    CHECK(values.x == 0.0F);
    CHECK(std::signbit(values.y));
    CHECK(values.z == 1.0F);
    CHECK(values.w == -2.0F);

    const auto special = round_trip<vng::gfx::f16x2>({
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(),
    });
    CHECK(std::isinf(special.x));
    CHECK(std::isnan(special.y));
}

TEST_CASE("normalized codecs clamp and round at their representable limits", "[gfx][codec]")
{
    const auto unorm2 = round_trip<vng::gfx::unorm8x2>({-1.0F, 2.0F});
    CHECK(unorm2 == vng::Vec2{0.0F, 1.0F});

    const auto unorm4 = round_trip<vng::gfx::unorm8x4>({0.0F, 0.25F, 0.5F, 1.0F});
    CHECK(unorm4.x == 0.0F);
    CHECK(close(unorm4.y, 0.25F, 1.0F / 255.0F));
    CHECK(close(unorm4.z, 0.5F, 1.0F / 255.0F));
    CHECK(unorm4.w == 1.0F);

    const auto snorm2 = round_trip<vng::gfx::snorm8x2>({-2.0F, 2.0F});
    CHECK(snorm2 == vng::Vec2{-1.0F, 1.0F});

    const auto snorm4 = round_trip<vng::gfx::snorm8x4>({-2.0F, -0.5F, 0.5F, 2.0F});
    CHECK(snorm4.x == -1.0F);
    CHECK(close(snorm4.y, -0.5F, 1.0F / 127.0F));
    CHECK(close(snorm4.z, 0.5F, 1.0F / 127.0F));
    CHECK(snorm4.w == 1.0F);

    const auto packed = round_trip<vng::gfx::snorm10x3>({-2.0F, 0.25F, 2.0F});
    CHECK(packed.x == -1.0F);
    CHECK(close(packed.y, 0.25F, 1.0F / 511.0F));
    CHECK(packed.z == 1.0F);

    const auto nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(round_trip<vng::gfx::unorm8x2>({nan, nan}) == vng::Vec2{});
    CHECK(round_trip<vng::gfx::snorm8x4>({nan, nan, nan, nan}) == vng::Vec4{});
    CHECK(round_trip<vng::gfx::snorm10x3>({nan, nan, nan}) == vng::Vec3{});
}

TEST_CASE("vertex streams and layouts retain binding and instance metadata", "[gfx][layout]")
{
    PackedVertex vertex;
    vertex.set(ObjectId{}, 0xFEED'1234U);
    vng::gfx::VertexStream<PackedVertex> stream(2);
    stream[0] = vertex;
    CHECK(stream.bytes().size() == 2 * PackedVertex::stride);
    CHECK(stream[0].get(ObjectId{}) == 0xFEED'1234U);

    const auto resolved = vng::gfx::resolve_vertex_input<ShaderInputs, Layout>();
    REQUIRE(resolved.attributes.size() == 4);
    CHECK(resolved.stream_count == 2);
    CHECK(resolved.attributes[0].location == 0);
    CHECK(resolved.attributes[0].offset == PackedVertex::offset(Position{}));
    CHECK(resolved.attributes[1].location == 1);
    CHECK(resolved.attributes[1].format == vng::gfx::unorm8x4::format);
    CHECK(resolved.attributes[2].binding == 1);
    CHECK(resolved.attributes[2].divisor == 1);
    CHECK_FALSE(vng::gfx::dump_vertex_layout(resolved).empty());
}

TEST_CASE("neutral vertex formats validate their physical and logical contracts", "[gfx][format]")
{
    auto wrong_size = vng::gfx::f32x3::format;
    --wrong_size.byte_size;
    CHECK_FALSE(vng::gfx::valid_vertex_format(wrong_size));
    CHECK_FALSE(vng::gfx::vertex_format_compatible<vng::Vec3>(wrong_size));

    auto wrong_storage_count = vng::gfx::snorm10x3::format;
    wrong_storage_count.storage_component_count = 3;
    CHECK_FALSE(vng::gfx::valid_vertex_format(wrong_storage_count));

    auto integer_float = vng::gfx::f32x4::format;
    integer_float.interpretation = vng::gfx::AttributeInterpretation::Integer;
    CHECK_FALSE(vng::gfx::valid_vertex_format(integer_float));

    auto signed_for_unsigned = vng::gfx::i32x2::format;
    CHECK(vng::gfx::valid_vertex_format(signed_for_unsigned));
    CHECK_FALSE(vng::gfx::vertex_format_compatible<vng::UVec2>(signed_for_unsigned));

    auto normalized_integer = vng::gfx::unorm8x4::format;
    CHECK(vng::gfx::vertex_format_compatible<vng::Vec4>(normalized_integer));
    CHECK_FALSE(vng::gfx::vertex_format_compatible<vng::UVec4>(normalized_integer));
}
