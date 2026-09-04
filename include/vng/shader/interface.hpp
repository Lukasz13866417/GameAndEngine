#pragma once

#include <vng/core/types.hpp>
#include <vng/gfx/semantic.hpp>

#include <cstddef>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace vng::shader {

enum class StageKind : std::uint8_t {
    vertex,
    fragment,
};

enum class InterfaceDirection : std::uint8_t {
    input,
    output,
};

enum class Interpolation : std::uint8_t {
    none,
    smooth,
    flat,
    no_perspective,
};

enum class Builtin : std::uint8_t {
    none,
    clip_position,
    fragment_depth,
    fragment_coordinate,
    front_facing,
    vertex_index,
    instance_index,
    color,
};

struct ClipPosition : gfx::Semantic<Vec4> {};
struct FragmentDepth : gfx::Semantic<f32> {};
struct FragmentCoordinate : gfx::Semantic<Vec4> {};
struct FrontFacing : gfx::Semantic<bool> {};
struct VertexIndex : gfx::Semantic<i32> {};
struct InstanceIndex : gfx::Semantic<i32> {};

template<u32 Index>
struct Color : gfx::Semantic<Vec4> {
    static constexpr u32 index = Index;
};

template<class Semantic>
struct smooth {
    using semantic_type = Semantic;
};

template<class Semantic>
struct flat {
    using semantic_type = Semantic;
};

template<class Semantic>
struct no_perspective {
    using semantic_type = Semantic;
};

namespace detail {

template<class Field>
struct interface_field_traits {
    using semantic_type = Field;
    static constexpr Interpolation interpolation = Interpolation::none;
};

template<class Semantic>
struct interface_field_traits<smooth<Semantic>> {
    using semantic_type = Semantic;
    static constexpr Interpolation interpolation = Interpolation::smooth;
};

template<class Semantic>
struct interface_field_traits<flat<Semantic>> {
    using semantic_type = Semantic;
    static constexpr Interpolation interpolation = Interpolation::flat;
};

template<class Semantic>
struct interface_field_traits<no_perspective<Semantic>> {
    using semantic_type = Semantic;
    static constexpr Interpolation interpolation = Interpolation::no_perspective;
};

template<class Field>
using interface_semantic_t = typename interface_field_traits<Field>::semantic_type;

template<class T>
struct builtin_traits {
    static constexpr Builtin value = Builtin::none;
    static constexpr u32 index = 0;
};

template<>
struct builtin_traits<ClipPosition> {
    static constexpr Builtin value = Builtin::clip_position;
    static constexpr u32 index = 0;
};

template<>
struct builtin_traits<FragmentDepth> {
    static constexpr Builtin value = Builtin::fragment_depth;
    static constexpr u32 index = 0;
};

template<>
struct builtin_traits<FragmentCoordinate> {
    static constexpr Builtin value = Builtin::fragment_coordinate;
    static constexpr u32 index = 0;
};

template<>
struct builtin_traits<FrontFacing> {
    static constexpr Builtin value = Builtin::front_facing;
    static constexpr u32 index = 0;
};

template<>
struct builtin_traits<VertexIndex> {
    static constexpr Builtin value = Builtin::vertex_index;
    static constexpr u32 index = 0;
};

template<>
struct builtin_traits<InstanceIndex> {
    static constexpr Builtin value = Builtin::instance_index;
    static constexpr u32 index = 0;
};

template<u32 Index>
struct builtin_traits<Color<Index>> {
    static constexpr Builtin value = Builtin::color;
    static constexpr u32 index = Index;
};

template<class... Ts>
consteval bool unique_types()
{
    if constexpr (sizeof...(Ts) < 2) {
        return true;
    } else {
        return []<class Head, class... Tail>(gfx::TypeList<Head, Tail...>) {
            return ((!std::is_same_v<Head, Tail>) && ...) && unique_types<Tail...>();
        }(gfx::TypeList<Ts...>{});
    }
}

template<StageKind Stage, InterfaceDirection Direction, class... Fields>
struct InterfaceBase {
    using declared_fields = gfx::TypeList<Fields...>;
    using semantics = gfx::TypeList<interface_semantic_t<Fields>...>;

    static constexpr StageKind stage = Stage;
    static constexpr InterfaceDirection direction = Direction;
    static constexpr std::size_t field_count = sizeof...(Fields);

    static_assert((gfx::SemanticType<interface_semantic_t<Fields>> && ...),
                  "every shader interface field must be a semantic tag or interpolation-wrapped semantic tag");
    static_assert(unique_types<interface_semantic_t<Fields>...>(),
                  "a shader interface cannot declare the same semantic twice");
};

} // namespace detail

template<class... Fields>
struct VertexInputs : detail::InterfaceBase<StageKind::vertex, InterfaceDirection::input, Fields...> {
    static_assert(((detail::builtin_traits<detail::interface_semantic_t<Fields>>::value == Builtin::none) && ...),
                  "VertexInputs contains only buffer-backed attributes; use the stage's builtin accessors");
};

template<class... Fields>
struct VertexOutputs : detail::InterfaceBase<StageKind::vertex, InterfaceDirection::output, Fields...> {};

template<class... Fields>
struct FragmentInputs : detail::InterfaceBase<StageKind::fragment, InterfaceDirection::input, Fields...> {};

template<class... Fields>
struct FragmentOutputs : detail::InterfaceBase<StageKind::fragment, InterfaceDirection::output, Fields...> {};

template<class T>
concept Interface = requires {
    typename T::declared_fields;
    typename T::semantics;
    { T::stage } -> std::convertible_to<StageKind>;
    { T::direction } -> std::convertible_to<InterfaceDirection>;
    { T::field_count } -> std::convertible_to<std::size_t>;
};

} // namespace vng::shader
