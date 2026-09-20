#pragma once

#include <array>
#include <bit>
#include <span>
#include <tuple>
#include <typeindex>
#include <type_traits>

#include <vng/shader/type.hpp>

namespace vng::shader {

// Interfaces describe stage IO, not CPU argument values. Supported records
// are decoded field-by-field; their vertex-storage encoding is never an ABI.
template<class T>
concept Argument = Value<T> && (!Interface<std::remove_cvref_t<T>>);

template<Argument... Ts>
struct Arguments {};

struct ArgumentView final {
    std::type_index type{typeid(void)};
    std::span<const u32> words;
};

namespace detail {

template<class T>
consteval std::size_t argument_word_count()
{
    if constexpr (is_scalar_value_v<T>) return 1;
    else if constexpr (is_vector_value_v<T>) return vector_traits<T>::component_count;
    else if constexpr (is_matrix_v<T>) return matrix_traits<T>::columns * matrix_traits<T>::rows;
    else {
        std::size_t count = 0;
        for_each_type<typename record_traits<T>::semantics>([&]<class Tag> {
            count += argument_word_count<typename record_traits<T>::template value_type<Tag>>();
        });
        return count;
    }
}

template<class T>
void append_argument_words(const T& value, u32*& output)
{
    if constexpr (std::same_as<T, bool>) *output++ = value ? 1U : 0U;
    else if constexpr (is_scalar_value_v<T>) *output++ = std::bit_cast<u32>(value);
    else if constexpr (is_vector_value_v<T>) {
        for (std::size_t i = 0; i < vector_traits<T>::component_count; ++i)
            append_argument_words(value[i], output);
    } else if constexpr (is_matrix_v<T>) {
        for (std::size_t column = 0; column < matrix_traits<T>::columns; ++column)
            for (std::size_t row = 0; row < matrix_traits<T>::rows; ++row)
                append_argument_words(value[column][row], output);
    } else {
        for_each_type<typename record_traits<T>::semantics>([&]<class Tag> {
            append_argument_words(value.get(Tag{}), output);
        });
    }
}

template<Argument T>
auto argument_words(const T& value)
{
    std::array<u32, argument_word_count<T>()> words{};
    auto* next = words.data();
    append_argument_words(value, next);
    return words;
}

template<class Signature, class... Values>
struct arguments_match : std::false_type {};

template<class... Ts, class... Values>
struct arguments_match<Arguments<Ts...>, Values...>
    : std::bool_constant<std::same_as<std::tuple<Ts...>,
          std::tuple<std::remove_cvref_t<Values>...>>> {};

} // namespace detail

template<class Signature, class... Values>
inline constexpr bool arguments_match_v = detail::arguments_match<Signature, Values...>::value;

// Stack-owned, tightly packed logical values. A backend consumes views before
// this pack dies, or makes an owned snapshot. No per-draw heap allocation and
// no references to caller data are needed to prepare the values.
template<Argument... Ts>
class ArgumentPack final {
public:
    explicit ArgumentPack(const Ts&... values) : words_(detail::argument_words(values)...) {}

    [[nodiscard]] std::array<ArgumentView, sizeof...(Ts)> views() const & noexcept
    {
        return std::apply([](const auto&... words) {
            return std::array<ArgumentView, sizeof...(Ts)>{ArgumentView{typeid(Ts), words}...};
        }, words_);
    }
    std::array<ArgumentView, sizeof...(Ts)> views() const && = delete;

private:
    std::tuple<std::array<u32, detail::argument_word_count<Ts>()>...> words_;
};

// Opt-in at shader::link: corresponding arguments of both stages represent
// the same values. Without this tag, stage argument lists are concatenated.
struct SharedArguments final {};
inline constexpr SharedArguments shared_arguments{};

} // namespace vng::shader
