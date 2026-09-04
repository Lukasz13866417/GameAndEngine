#pragma once

#include <vng/core/types.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace vng::gfx {

// Describes how an attribute is stored. Backends translate this descriptor to
// their own format enums; no OpenGL types leak into the schema layer.
enum class ComponentEncoding : std::uint8_t {
    Float16,
    Float32,
    Signed8,
    Unsigned8,
    Signed32,
    Unsigned32,
    Signed2_10_10_10Rev,
};

enum class AttributeInterpretation : std::uint8_t {
    FloatingPoint,
    Normalized,
    Integer,
};

struct VertexFormat {
    ComponentEncoding encoding{};
    AttributeInterpretation interpretation{};

    // Number of components visible to the shader.
    std::uint8_t component_count{};

    // Number encoded by the physical representation. This differs for a
    // three-component value stored as 2:10:10:10.
    std::uint8_t storage_component_count{};

    std::uint16_t byte_size{};
    std::uint8_t byte_alignment{};

    [[nodiscard]] constexpr bool integer_delivery() const noexcept
    {
        return interpretation == AttributeInterpretation::Integer;
    }

    [[nodiscard]] constexpr bool normalized() const noexcept
    {
        return interpretation == AttributeInterpretation::Normalized;
    }

    friend constexpr bool operator==(const VertexFormat&, const VertexFormat&) = default;
};

namespace detail {

template<class T>
struct vertex_attribute_value_traits {
    static constexpr bool supported = false;
};

template<>
struct vertex_attribute_value_traits<f32> {
    using scalar_type = f32;
    static constexpr std::uint8_t component_count = 1;
    static constexpr bool supported = true;
};

template<>
struct vertex_attribute_value_traits<i32> {
    using scalar_type = i32;
    static constexpr std::uint8_t component_count = 1;
    static constexpr bool supported = true;
};

template<>
struct vertex_attribute_value_traits<u32> {
    using scalar_type = u32;
    static constexpr std::uint8_t component_count = 1;
    static constexpr bool supported = true;
};

template<class Scalar, std::size_t Components>
    requires (Components >= 2 && Components <= 4) &&
             (std::same_as<Scalar, f32> || std::same_as<Scalar, i32> ||
              std::same_as<Scalar, u32>)
struct vertex_attribute_value_traits<Vector<Scalar, Components>> {
    using scalar_type = Scalar;
    static constexpr std::uint8_t component_count =
        static_cast<std::uint8_t>(Components);
    static constexpr bool supported = true;
};

[[nodiscard]] constexpr bool valid_component_encoding(ComponentEncoding encoding) noexcept
{
    switch (encoding) {
    case ComponentEncoding::Float16:
    case ComponentEncoding::Float32:
    case ComponentEncoding::Signed8:
    case ComponentEncoding::Unsigned8:
    case ComponentEncoding::Signed32:
    case ComponentEncoding::Unsigned32:
    case ComponentEncoding::Signed2_10_10_10Rev:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool valid_attribute_interpretation(
    AttributeInterpretation interpretation) noexcept
{
    switch (interpretation) {
    case AttributeInterpretation::FloatingPoint:
    case AttributeInterpretation::Normalized:
    case AttributeInterpretation::Integer:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr std::uint16_t encoded_byte_size(
    ComponentEncoding encoding,
    std::uint8_t storage_components) noexcept
{
    switch (encoding) {
    case ComponentEncoding::Float16:
        return static_cast<std::uint16_t>(2U * storage_components);
    case ComponentEncoding::Float32:
    case ComponentEncoding::Signed32:
    case ComponentEncoding::Unsigned32:
        return static_cast<std::uint16_t>(4U * storage_components);
    case ComponentEncoding::Signed8:
    case ComponentEncoding::Unsigned8:
        return storage_components;
    case ComponentEncoding::Signed2_10_10_10Rev:
        return 4;
    }
    return 0;
}

[[nodiscard]] constexpr std::uint8_t encoded_byte_alignment(
    ComponentEncoding encoding) noexcept
{
    switch (encoding) {
    case ComponentEncoding::Float16:
        return 2;
    case ComponentEncoding::Float32:
    case ComponentEncoding::Signed32:
    case ComponentEncoding::Unsigned32:
    case ComponentEncoding::Signed2_10_10_10Rev:
        return 4;
    case ComponentEncoding::Signed8:
    case ComponentEncoding::Unsigned8:
        return 1;
    }
    return 0;
}

} // namespace detail

// Checks the self-contained physical contract. This is also used at erased
// backend boundaries, where the original logical C++ type is no longer known.
[[nodiscard]] constexpr bool valid_vertex_format(VertexFormat format) noexcept
{
    if (!detail::valid_component_encoding(format.encoding) ||
        !detail::valid_attribute_interpretation(format.interpretation) ||
        format.component_count < 1 || format.component_count > 4) {
        return false;
    }

    const bool packed =
        format.encoding == ComponentEncoding::Signed2_10_10_10Rev;
    if ((packed && (format.storage_component_count != 4 ||
                    (format.component_count != 3 && format.component_count != 4))) ||
        (!packed && format.storage_component_count != format.component_count)) {
        return false;
    }

    const bool floating_storage = format.encoding == ComponentEncoding::Float16 ||
                                  format.encoding == ComponentEncoding::Float32;
    if ((floating_storage &&
         format.interpretation != AttributeInterpretation::FloatingPoint) ||
        (packed && format.interpretation == AttributeInterpretation::Integer)) {
        return false;
    }

    return format.byte_size ==
               detail::encoded_byte_size(format.encoding, format.storage_component_count) &&
           format.byte_alignment == detail::encoded_byte_alignment(format.encoding);
}

// A logical vertex value is deliberately limited to f32/i32/u32 scalars and
// their 2-4 component vectors in this milestone. Matrices need one OpenGL
// attribute per column, while booleans and records are not legal vertex
// attributes, so accepting any of them as one codec field would be misleading.
template<class Value>
[[nodiscard]] constexpr bool vertex_format_compatible(VertexFormat format) noexcept
{
    using Logical = std::remove_cvref_t<Value>;
    using Traits = detail::vertex_attribute_value_traits<Logical>;
    if constexpr (!Traits::supported) {
        return false;
    } else {
        if (!valid_vertex_format(format) ||
            format.component_count != Traits::component_count) {
            return false;
        }

        using Scalar = typename Traits::scalar_type;
        if constexpr (std::same_as<Scalar, f32>) {
            return format.interpretation != AttributeInterpretation::Integer;
        } else if constexpr (std::same_as<Scalar, i32>) {
            return format.interpretation == AttributeInterpretation::Integer &&
                   (format.encoding == ComponentEncoding::Signed8 ||
                    format.encoding == ComponentEncoding::Signed32);
        } else {
            return format.interpretation == AttributeInterpretation::Integer &&
                   (format.encoding == ComponentEncoding::Unsigned8 ||
                    format.encoding == ComponentEncoding::Unsigned32);
        }
    }
}

} // namespace vng::gfx
