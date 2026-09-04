#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include <vng/core/types.hpp>
#include <vng/gfx/format.hpp>

namespace vng::gfx {

namespace detail {

template<class Scalar, std::size_t Components>
struct codec_value {
    using type = Vector<Scalar, Components>;
};

template<class Scalar>
struct codec_value<Scalar, 1> {
    using type = Scalar;
};

template<class Scalar, std::size_t Components>
using codec_value_t = typename codec_value<Scalar, Components>::type;

template<class Value>
[[nodiscard]] constexpr auto component(const Value& value, std::size_t index) noexcept
{
    if constexpr (is_vector_v<Value>) {
        return value[index];
    } else {
        (void)index;
        return value;
    }
}

template<class Value, class Scalar, std::size_t Components>
[[nodiscard]] constexpr Value from_components(const std::array<Scalar, Components>& components) noexcept
{
    if constexpr (Components == 1) {
        return components[0];
    } else {
        Value result{};
        for (std::size_t index = 0; index < Components; ++index) {
            result[index] = components[index];
        }
        return result;
    }
}

template<class Scalar,
         std::size_t Components,
         ComponentEncoding Encoding,
         AttributeInterpretation Interpretation>
struct raw_codec {
    using value_type = codec_value_t<Scalar, Components>;
    using logical_type = value_type;
    using storage_type = std::array<Scalar, Components>;

    static constexpr std::size_t size = sizeof(Scalar) * Components;
    static constexpr std::size_t alignment = alignof(Scalar);
    static constexpr VertexFormat format{
        Encoding,
        Interpretation,
        static_cast<std::uint8_t>(Components),
        static_cast<std::uint8_t>(Components),
        static_cast<std::uint16_t>(size),
        static_cast<std::uint8_t>(alignment),
    };

    static void encode(std::byte* destination, const value_type& value) noexcept
    {
        storage_type storage{};
        for (std::size_t index = 0; index < Components; ++index) {
            storage[index] = component(value, index);
        }
        std::memcpy(destination, storage.data(), size);
    }

    [[nodiscard]] static value_type decode(const std::byte* source) noexcept
    {
        storage_type storage{};
        std::memcpy(storage.data(), source, size);
        return from_components<value_type>(storage);
    }
};

[[nodiscard]] inline std::uint16_t float_to_half(float value) noexcept
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const std::uint32_t exponent = (bits >> 23U) & 0xFFU;
    std::uint32_t mantissa = bits & 0x7FFFFFU;

    if (exponent == 0xFFU) {
        if (mantissa == 0) {
            return static_cast<std::uint16_t>(sign | 0x7C00U);
        }
        // Preserve some payload while ensuring the result remains a NaN.
        return static_cast<std::uint16_t>(sign | 0x7C00U | (mantissa >> 13U) | 1U);
    }

    const int half_exponent = static_cast<int>(exponent) - 127 + 15;
    if (half_exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    }

    if (half_exponent <= 0) {
        if (half_exponent < -10) {
            return static_cast<std::uint16_t>(sign);
        }

        mantissa |= 0x800000U;
        const unsigned shift = static_cast<unsigned>(14 - half_exponent);
        std::uint32_t rounded = mantissa >> shift;
        const std::uint32_t remainder_mask = (1U << shift) - 1U;
        const std::uint32_t remainder = mantissa & remainder_mask;
        const std::uint32_t halfway = 1U << (shift - 1U);
        if (remainder > halfway || (remainder == halfway && (rounded & 1U) != 0U)) {
            ++rounded;
        }
        return static_cast<std::uint16_t>(sign | rounded);
    }

    std::uint32_t result = sign | (static_cast<std::uint32_t>(half_exponent) << 10U)
                           | (mantissa >> 13U);
    const std::uint32_t remainder = mantissa & 0x1FFFU;
    if (remainder > 0x1000U || (remainder == 0x1000U && (result & 1U) != 0U)) {
        ++result;
    }
    return static_cast<std::uint16_t>(result);
}

[[nodiscard]] inline float half_to_float(std::uint16_t half) noexcept
{
    const std::uint32_t sign = (static_cast<std::uint32_t>(half & 0x8000U)) << 16U;
    std::uint32_t exponent = (half >> 10U) & 0x1FU;
    std::uint32_t mantissa = half & 0x03FFU;
    std::uint32_t result{};

    if (exponent == 0) {
        if (mantissa == 0) {
            result = sign;
        } else {
            // Normalize a half subnormal.
            int shift = 0;
            while ((mantissa & 0x0400U) == 0U) {
                mantissa <<= 1U;
                ++shift;
            }
            mantissa &= 0x03FFU;
            const auto float_exponent = static_cast<std::uint32_t>(127 - 15 + 1 - shift);
            result = sign | (float_exponent << 23U) | (mantissa << 13U);
        }
    } else if (exponent == 0x1FU) {
        result = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        exponent = exponent + (127U - 15U);
        result = sign | (exponent << 23U) | (mantissa << 13U);
    }

    return std::bit_cast<float>(result);
}

template<std::size_t Components>
struct half_codec {
    static_assert(Components == 2 || Components == 4);

    using value_type = codec_value_t<f32, Components>;
    using logical_type = value_type;
    using storage_type = std::array<std::uint16_t, Components>;

    static constexpr std::size_t size = sizeof(std::uint16_t) * Components;
    static constexpr std::size_t alignment = alignof(std::uint16_t);
    static constexpr VertexFormat format{
        ComponentEncoding::Float16,
        AttributeInterpretation::FloatingPoint,
        static_cast<std::uint8_t>(Components),
        static_cast<std::uint8_t>(Components),
        static_cast<std::uint16_t>(size),
        static_cast<std::uint8_t>(alignment),
    };

    static void encode(std::byte* destination, const value_type& value) noexcept
    {
        storage_type storage{};
        for (std::size_t index = 0; index < Components; ++index) {
            storage[index] = float_to_half(component(value, index));
        }
        std::memcpy(destination, storage.data(), size);
    }

    [[nodiscard]] static value_type decode(const std::byte* source) noexcept
    {
        storage_type storage{};
        std::memcpy(storage.data(), source, size);
        std::array<f32, Components> values{};
        for (std::size_t index = 0; index < Components; ++index) {
            values[index] = half_to_float(storage[index]);
        }
        return from_components<value_type>(values);
    }
};

template<std::size_t Components, bool Signed>
struct normalized_byte_codec {
    static_assert(Components == 2 || Components == 4);

    using storage_scalar = std::conditional_t<Signed, std::int8_t, std::uint8_t>;
    using value_type = codec_value_t<f32, Components>;
    using logical_type = value_type;
    using storage_type = std::array<storage_scalar, Components>;

    static constexpr std::size_t size = Components;
    static constexpr std::size_t alignment = 1;
    static constexpr VertexFormat format{
        Signed ? ComponentEncoding::Signed8 : ComponentEncoding::Unsigned8,
        AttributeInterpretation::Normalized,
        static_cast<std::uint8_t>(Components),
        static_cast<std::uint8_t>(Components),
        static_cast<std::uint16_t>(size),
        static_cast<std::uint8_t>(alignment),
    };

    static void encode(std::byte* destination, const value_type& value) noexcept
    {
        storage_type storage{};
        for (std::size_t index = 0; index < Components; ++index) {
            const f32 raw = component(value, index);
            // NaN has no useful normalized integer representation. Defining it
            // as zero keeps encoding deterministic and avoids an out-of-range
            // floating-to-integer conversion inside lround.
            const f32 source = std::isnan(raw) ? 0.0F : raw;
            if constexpr (Signed) {
                storage[index] = static_cast<std::int8_t>(
                    std::lround(std::clamp(source, -1.0F, 1.0F) * 127.0F));
            } else {
                storage[index] = static_cast<std::uint8_t>(
                    std::lround(std::clamp(source, 0.0F, 1.0F) * 255.0F));
            }
        }
        std::memcpy(destination, storage.data(), size);
    }

    [[nodiscard]] static value_type decode(const std::byte* source) noexcept
    {
        storage_type storage{};
        std::memcpy(storage.data(), source, size);
        std::array<f32, Components> values{};
        for (std::size_t index = 0; index < Components; ++index) {
            if constexpr (Signed) {
                values[index] = std::max(-1.0F, static_cast<f32>(storage[index]) / 127.0F);
            } else {
                values[index] = static_cast<f32>(storage[index]) / 255.0F;
            }
        }
        return from_components<value_type>(values);
    }
};

} // namespace detail

using f32x1 = detail::raw_codec<f32, 1, ComponentEncoding::Float32,
                                AttributeInterpretation::FloatingPoint>;
using f32x2 = detail::raw_codec<f32, 2, ComponentEncoding::Float32,
                                AttributeInterpretation::FloatingPoint>;
using f32x3 = detail::raw_codec<f32, 3, ComponentEncoding::Float32,
                                AttributeInterpretation::FloatingPoint>;
using f32x4 = detail::raw_codec<f32, 4, ComponentEncoding::Float32,
                                AttributeInterpretation::FloatingPoint>;

using i32x1 = detail::raw_codec<i32, 1, ComponentEncoding::Signed32,
                                AttributeInterpretation::Integer>;
using i32x2 = detail::raw_codec<i32, 2, ComponentEncoding::Signed32,
                                AttributeInterpretation::Integer>;
using i32x3 = detail::raw_codec<i32, 3, ComponentEncoding::Signed32,
                                AttributeInterpretation::Integer>;
using i32x4 = detail::raw_codec<i32, 4, ComponentEncoding::Signed32,
                                AttributeInterpretation::Integer>;

using u32x1 = detail::raw_codec<u32, 1, ComponentEncoding::Unsigned32,
                                AttributeInterpretation::Integer>;
using u32x2 = detail::raw_codec<u32, 2, ComponentEncoding::Unsigned32,
                                AttributeInterpretation::Integer>;
using u32x3 = detail::raw_codec<u32, 3, ComponentEncoding::Unsigned32,
                                AttributeInterpretation::Integer>;
using u32x4 = detail::raw_codec<u32, 4, ComponentEncoding::Unsigned32,
                                AttributeInterpretation::Integer>;

using f16x2 = detail::half_codec<2>;
using f16x4 = detail::half_codec<4>;

using unorm8x2 = detail::normalized_byte_codec<2, false>;
using unorm8x4 = detail::normalized_byte_codec<4, false>;
using snorm8x2 = detail::normalized_byte_codec<2, true>;
using snorm8x4 = detail::normalized_byte_codec<4, true>;

struct snorm10x3 {
    using value_type = Vec3;
    using logical_type = value_type;
    using storage_type = std::uint32_t;

    static constexpr std::size_t size = sizeof(storage_type);
    static constexpr std::size_t alignment = alignof(storage_type);
    static constexpr VertexFormat format{
        ComponentEncoding::Signed2_10_10_10Rev,
        AttributeInterpretation::Normalized,
        3,
        4,
        static_cast<std::uint16_t>(size),
        static_cast<std::uint8_t>(alignment),
    };

    static void encode(std::byte* destination, const value_type& value) noexcept
    {
        std::uint32_t packed = 0;
        for (std::size_t index = 0; index < 3; ++index) {
            const auto source = std::isnan(value[index]) ? 0.0F : value[index];
            const auto quantized = static_cast<std::int32_t>(
                std::lround(std::clamp(source, -1.0F, 1.0F) * 511.0F));
            packed |= (static_cast<std::uint32_t>(quantized) & 0x3FFU)
                      << static_cast<unsigned>(index * 10);
        }
        std::memcpy(destination, &packed, size);
    }

    [[nodiscard]] static value_type decode(const std::byte* source) noexcept
    {
        std::uint32_t packed{};
        std::memcpy(&packed, source, size);
        value_type result{};
        for (std::size_t index = 0; index < 3; ++index) {
            std::int32_t component = static_cast<std::int32_t>(
                (packed >> static_cast<unsigned>(index * 10)) & 0x3FFU);
            if ((component & 0x200) != 0) {
                component |= ~0x3FF;
            }
            result[index] = std::max(-1.0F, static_cast<f32>(component) / 511.0F);
        }
        return result;
    }
};

template<class Codec>
concept VertexCodec = requires(std::byte* output, const std::byte* input,
                               const typename Codec::value_type& value) {
    typename Codec::logical_type;
    requires std::same_as<typename Codec::value_type, typename Codec::logical_type>;
    { Codec::size } -> std::convertible_to<std::size_t>;
    { Codec::alignment } -> std::convertible_to<std::size_t>;
    { Codec::format } -> std::convertible_to<VertexFormat>;
    { Codec::encode(output, value) } noexcept -> std::same_as<void>;
    { Codec::decode(input) } noexcept -> std::same_as<typename Codec::value_type>;
};

template<class Codec, class Value>
concept CodecFor = VertexCodec<Codec> && std::same_as<typename Codec::value_type, Value>;

template<class Value>
struct default_codec;

template<>
struct default_codec<f32> { using type = f32x1; };
template<>
struct default_codec<Vec2> { using type = f32x2; };
template<>
struct default_codec<Vec3> { using type = f32x3; };
template<>
struct default_codec<Vec4> { using type = f32x4; };

template<>
struct default_codec<i32> { using type = i32x1; };
template<>
struct default_codec<IVec2> { using type = i32x2; };
template<>
struct default_codec<IVec3> { using type = i32x3; };
template<>
struct default_codec<IVec4> { using type = i32x4; };

template<>
struct default_codec<u32> { using type = u32x1; };
template<>
struct default_codec<UVec2> { using type = u32x2; };
template<>
struct default_codec<UVec3> { using type = u32x3; };
template<>
struct default_codec<UVec4> { using type = u32x4; };

template<class Value>
using default_codec_t = typename default_codec<Value>::type;

} // namespace vng::gfx
