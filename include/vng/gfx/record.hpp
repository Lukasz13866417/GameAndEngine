#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

#include <vng/gfx/codecs.hpp>
#include <vng/gfx/semantic.hpp>

namespace vng::gfx {

namespace detail {

template<class Field, bool HasExplicitCodec = field_has_explicit_codec_v<Field>>
struct resolved_field_codec;

template<class Field>
struct resolved_field_codec<Field, false> {
    using type = default_codec_t<semantic_value_t<field_semantic_t<Field>>>;
};

template<class Field>
struct resolved_field_codec<Field, true> {
    using type = explicit_field_codec_t<Field>;
};

template<class Field>
using resolved_field_codec_t = typename resolved_field_codec<Field>::type;

template<class Field>
struct record_field_traits {
    using semantic_type = field_semantic_t<Field>;
    using value_type = semantic_value_t<semantic_type>;
    using codec_type = resolved_field_codec_t<Field>;

    static_assert(VertexCodec<codec_type>, "A Record field encoding must satisfy VertexCodec");
    static_assert(CodecFor<codec_type, value_type>,
                  "A Record field encoding must encode the semantic's logical value type");
    static_assert(codec_type::size != 0, "A vertex codec cannot have zero size");
    static_assert(codec_type::alignment != 0, "A vertex codec cannot have zero alignment");
    static_assert((codec_type::alignment & (codec_type::alignment - 1)) == 0,
                  "A vertex codec's alignment must be a power of two");
    static_assert(codec_type::format.byte_size == codec_type::size,
                  "VertexFormat::byte_size must match Codec::size");
    static_assert(codec_type::format.byte_alignment == codec_type::alignment,
                  "VertexFormat::byte_alignment must match Codec::alignment");
    static_assert(vertex_format_compatible<value_type>(codec_type::format),
                  "A Record field codec's VertexFormat is incompatible with its logical value type or physical format");
};

template<class Tag, class Head, class... Tail>
struct find_record_field;

template<bool Matches, class Tag, class Head, class... Tail>
struct find_record_field_step;

template<class Tag, class Head, class... Tail>
struct find_record_field_step<true, Tag, Head, Tail...> {
    using type = Head;
};

template<class Tag, class Head, class... Tail>
struct find_record_field_step<false, Tag, Head, Tail...> {
    using type = typename find_record_field<Tag, Tail...>::type;
};

template<class Tag, class Head, class... Tail>
struct find_record_field
    : find_record_field_step<std::same_as<Tag, field_semantic_t<Head>>,
                             Tag, Head, Tail...> {};

template<class Tag, class Head>
struct find_record_field<Tag, Head> {
    static_assert(std::same_as<Tag, field_semantic_t<Head>>, "Semantic is not present in Record");
    using type = Head;
};

template<class Tag, class... Fields>
using find_record_field_t = typename find_record_field<Tag, Fields...>::type;

[[nodiscard]] consteval std::size_t align_up(std::size_t value, std::size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

template<std::size_t FieldCount>
struct record_layout {
    std::array<std::size_t, FieldCount> offsets{};
    std::size_t stride{};
    std::size_t alignment{};
};

template<class... Fields>
[[nodiscard]] consteval auto calculate_record_layout()
{
    record_layout<sizeof...(Fields)> result{};
    constexpr std::array sizes{record_field_traits<Fields>::codec_type::size...};
    constexpr std::array alignments{record_field_traits<Fields>::codec_type::alignment...};

    std::size_t cursor = 0;
    std::size_t maximum_alignment = 1;
    for (std::size_t index = 0; index < sizeof...(Fields); ++index) {
        cursor = align_up(cursor, alignments[index]);
        result.offsets[index] = cursor;
        cursor += sizes[index];
        maximum_alignment = alignments[index] > maximum_alignment
                                ? alignments[index]
                                : maximum_alignment;
    }
    result.alignment = maximum_alignment;
    result.stride = align_up(cursor, maximum_alignment);
    return result;
}

} // namespace detail

template<class... Fields>
    requires (sizeof...(Fields) > 0)
             && (SemanticType<field_semantic_t<Fields>> && ...)
class Record {
    using field_tuple = std::tuple<Fields...>;
    inline static constexpr auto layout = detail::calculate_record_layout<Fields...>();

    template<class Function, std::size_t... Indices>
    static constexpr void for_each_field_impl(Function&& function,
                                              std::index_sequence<Indices...>)
    {
        (static_cast<void>(function(field_descriptor<Indices>{})), ...);
    }

public:
    using semantics = TypeList<field_semantic_t<Fields>...>;
    using field_specs = TypeList<Fields...>;

    static constexpr std::size_t field_count = sizeof...(Fields);
    static constexpr std::size_t stride = layout.stride;
    static constexpr std::size_t alignment = layout.alignment;

    static_assert(type_list_unique_v<semantics>,
                  "A semantic may occur only once in a Record");

    template<std::size_t Index>
    struct field_descriptor {
        static_assert(Index < field_count);
        using field_type = std::tuple_element_t<Index, field_tuple>;
        using traits = detail::record_field_traits<field_type>;
        using semantic_type = typename traits::semantic_type;
        using value_type = typename traits::value_type;
        using codec_type = typename traits::codec_type;

        static constexpr std::size_t index = Index;
        static constexpr std::size_t offset = layout.offsets[Index];
        static constexpr std::size_t size = codec_type::size;
        static constexpr VertexFormat format = codec_type::format;
    };

    template<class Tag>
    [[nodiscard]] static consteval bool has(Tag = {}) noexcept
    {
        return (std::same_as<std::remove_cvref_t<Tag>, field_semantic_t<Fields>> || ...);
    }

    template<class Tag>
        requires SemanticType<std::remove_cvref_t<Tag>>
                 && (has(std::remove_cvref_t<Tag>{}))
    using value_type_for = semantic_value_t<std::remove_cvref_t<Tag>>;

    template<class Tag>
        requires SemanticType<std::remove_cvref_t<Tag>>
                 && (has(std::remove_cvref_t<Tag>{}))
    using field_spec_for = detail::find_record_field_t<std::remove_cvref_t<Tag>, Fields...>;

    template<class Tag>
        requires SemanticType<std::remove_cvref_t<Tag>>
                 && (has(std::remove_cvref_t<Tag>{}))
    using codec_for = detail::resolved_field_codec_t<field_spec_for<Tag>>;

    template<class Tag>
        requires SemanticType<std::remove_cvref_t<Tag>>
                 && (has(std::remove_cvref_t<Tag>{}))
    [[nodiscard]] static consteval std::size_t offset(Tag = {}) noexcept
    {
        constexpr std::array matches{
            std::same_as<std::remove_cvref_t<Tag>, field_semantic_t<Fields>>...
        };
        for (std::size_t index = 0; index < matches.size(); ++index) {
            if (matches[index]) {
                return layout.offsets[index];
            }
        }
        return 0; // Unreachable because of the requires-clause.
    }

    template<class Function>
    static constexpr void for_each_field(Function&& function)
    {
        for_each_field_impl(
            std::forward<Function>(function),
            std::make_index_sequence<field_count>{});
    }

    template<class Tag>
        requires SemanticType<std::remove_cvref_t<Tag>>
                 && (has(std::remove_cvref_t<Tag>{}))
    [[nodiscard]] value_type_for<Tag> get(Tag = {}) const noexcept
    {
        using codec = codec_for<Tag>;
        return codec::decode(storage_.data() + offset(std::remove_cvref_t<Tag>{}));
    }

    template<class Tag>
        requires SemanticType<std::remove_cvref_t<Tag>>
                 && (has(std::remove_cvref_t<Tag>{}))
    void set(Tag, const value_type_for<Tag>& value) noexcept
    {
        using codec = codec_for<Tag>;
        codec::encode(storage_.data() + offset(std::remove_cvref_t<Tag>{}), value);
    }

    [[nodiscard]] std::span<std::byte, stride> bytes() noexcept
    {
        return storage_;
    }

    [[nodiscard]] std::span<const std::byte, stride> bytes() const noexcept
    {
        return storage_;
    }

    [[nodiscard]] std::byte* data() noexcept
    {
        return storage_.data();
    }

    [[nodiscard]] const std::byte* data() const noexcept
    {
        return storage_.data();
    }

private:
    std::array<std::byte, stride> storage_{};
};

template<class T>
struct is_record : std::false_type {};

template<class... Fields>
struct is_record<Record<Fields...>> : std::true_type {};

template<class T>
inline constexpr bool is_record_v = is_record<std::remove_cvref_t<T>>::value;

template<class RecordType, class Tag>
inline constexpr bool record_has_v = std::remove_cvref_t<RecordType>::has(Tag{});

template<class RecordType, class Tag>
using record_value_t = typename std::remove_cvref_t<RecordType>::template value_type_for<Tag>;

template<class RecordType, class Tag>
using record_codec_t = typename std::remove_cvref_t<RecordType>::template codec_for<Tag>;

template<class RecordType>
using record_semantics_t = typename std::remove_cvref_t<RecordType>::semantics;

template<class T>
concept RecordType = std::same_as<T, std::remove_cvref_t<T>> && is_record_v<T>;

} // namespace vng::gfx
