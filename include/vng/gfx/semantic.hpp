#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace vng::gfx {

template<class... Ts>
struct TypeList {
    static constexpr std::size_t size = sizeof...(Ts);
};

template<class List>
struct type_list_size;

template<class... Ts>
struct type_list_size<TypeList<Ts...>> : std::integral_constant<std::size_t, sizeof...(Ts)> {};

template<class List>
inline constexpr std::size_t type_list_size_v = type_list_size<List>::value;

template<class List, class T>
struct type_list_contains;

template<class... Ts, class T>
struct type_list_contains<TypeList<Ts...>, T>
    : std::bool_constant<(std::same_as<Ts, T> || ...)> {};

template<class List, class T>
inline constexpr bool type_list_contains_v = type_list_contains<List, T>::value;

template<class List>
struct type_list_unique;

template<>
struct type_list_unique<TypeList<>> : std::true_type {};

template<class Head, class... Tail>
struct type_list_unique<TypeList<Head, Tail...>>
    : std::bool_constant<
          !type_list_contains_v<TypeList<Tail...>, Head>
          && type_list_unique<TypeList<Tail...>>::value> {};

template<class List>
inline constexpr bool type_list_unique_v = type_list_unique<List>::value;

template<class Value>
struct Semantic {
    using value_type = Value;
};

template<class T>
concept SemanticType = requires {
    typename std::remove_cv_t<T>::value_type;
} && std::derived_from<
         std::remove_cv_t<T>,
         Semantic<typename std::remove_cv_t<T>::value_type>>;

template<SemanticType Tag>
using semantic_value_t = typename std::remove_cv_t<Tag>::value_type;

// Selects a non-default physical encoding for one occurrence of a semantic.
template<SemanticType Tag, class Codec>
struct as {
    using semantic_type = Tag;
    using codec_type = Codec;
};

template<class Field>
struct field_semantic {
    using type = Field;
};

template<SemanticType Tag, class Codec>
struct field_semantic<as<Tag, Codec>> {
    using type = Tag;
};

template<class Field>
using field_semantic_t = typename field_semantic<Field>::type;

template<class Field>
struct field_has_explicit_codec : std::false_type {};

template<SemanticType Tag, class Codec>
struct field_has_explicit_codec<as<Tag, Codec>> : std::true_type {};

template<class Field>
inline constexpr bool field_has_explicit_codec_v = field_has_explicit_codec<Field>::value;

template<class Field>
struct explicit_field_codec;

template<SemanticType Tag, class Codec>
struct explicit_field_codec<as<Tag, Codec>> {
    using type = Codec;
};

template<class Field>
using explicit_field_codec_t = typename explicit_field_codec<Field>::type;

} // namespace vng::gfx
