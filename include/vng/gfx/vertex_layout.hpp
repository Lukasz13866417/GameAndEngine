#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <vng/core/type_name.hpp>
#include <vng/core/types.hpp>
#include <vng/gfx/record.hpp>

namespace vng::gfx {

struct PerVertex {
    static constexpr u32 divisor = 0;
};

template<u32 Divisor = 1>
struct PerInstance {
    static_assert(Divisor > 0, "PerInstance's divisor must be greater than zero");
    static constexpr u32 divisor = Divisor;
};

template<class RecordType, class Rate = PerVertex>
    requires is_record_v<RecordType> && requires {
        { Rate::divisor } -> std::convertible_to<u32>;
    }
struct Stream {
    using record_type = RecordType;
    using rate = Rate;

    static constexpr u32 divisor = Rate::divisor;
    static constexpr std::size_t stride = RecordType::stride;
};

template<class T>
struct is_stream : std::false_type {};

template<class RecordType, class Rate>
struct is_stream<Stream<RecordType, Rate>> : std::true_type {};

template<class T>
inline constexpr bool is_stream_v = is_stream<std::remove_cv_t<T>>::value;

namespace detail {

template<class... Lists>
struct concatenate_type_lists;

template<>
struct concatenate_type_lists<> {
    using type = TypeList<>;
};

template<class... Ts>
struct concatenate_type_lists<TypeList<Ts...>> {
    using type = TypeList<Ts...>;
};

template<class... Left, class... Right, class... Tail>
struct concatenate_type_lists<TypeList<Left...>, TypeList<Right...>, Tail...>
    : concatenate_type_lists<TypeList<Left..., Right...>, Tail...> {};

template<class... Lists>
using concatenate_type_lists_t = typename concatenate_type_lists<Lists...>::type;

template<class T>
struct requirements_semantics {
    using type = typename T::semantics;
};

template<class... Tags>
struct requirements_semantics<TypeList<Tags...>> {
    using type = TypeList<Tags...>;
};

template<class T>
using requirements_semantics_t = typename requirements_semantics<T>::type;

template<class Layout, class RequirementList>
struct layout_satisfies_list;

template<class Layout, class... Tags>
struct layout_satisfies_list<Layout, TypeList<Tags...>>
    : std::bool_constant<(type_list_contains_v<typename Layout::semantics, Tags> && ...)> {};

template<class Tag, std::size_t Binding, class... Streams>
struct locate_stream;

template<bool Matches, class Tag, std::size_t Binding, class Head, class... Tail>
struct locate_stream_step;

template<class Tag, std::size_t Binding, class Head, class... Tail>
struct locate_stream_step<true, Tag, Binding, Head, Tail...> {
    using stream_type = Head;
    static constexpr std::size_t binding = Binding;
};

template<class Tag, std::size_t Binding, class Head, class... Tail>
struct locate_stream_step<false, Tag, Binding, Head, Tail...>
    : locate_stream<Tag, Binding + 1, Tail...> {};

template<class Tag, std::size_t Binding, class Head, class... Tail>
struct locate_stream<Tag, Binding, Head, Tail...>
    : locate_stream_step<Head::record_type::has(Tag{}), Tag, Binding, Head, Tail...> {};

template<class Tag, std::size_t Binding>
struct locate_stream<Tag, Binding> {
    static_assert(!std::same_as<Tag, Tag>, "Required semantic is not present in VertexLayout");
};

[[nodiscard]] constexpr std::string_view encoding_name(ComponentEncoding encoding) noexcept
{
    switch (encoding) {
    case ComponentEncoding::Float16: return "f16";
    case ComponentEncoding::Float32: return "f32";
    case ComponentEncoding::Signed8: return "i8";
    case ComponentEncoding::Unsigned8: return "u8";
    case ComponentEncoding::Signed32: return "i32";
    case ComponentEncoding::Unsigned32: return "u32";
    case ComponentEncoding::Signed2_10_10_10Rev: return "i2_10_10_10_rev";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view interpretation_name(
    AttributeInterpretation interpretation) noexcept
{
    switch (interpretation) {
    case AttributeInterpretation::FloatingPoint: return "float";
    case AttributeInterpretation::Normalized: return "normalized";
    case AttributeInterpretation::Integer: return "integer";
    }
    return "unknown";
}

} // namespace detail

template<class... Streams>
    requires (sizeof...(Streams) > 0) && (is_stream_v<Streams> && ...)
struct VertexLayout {
    using streams = TypeList<Streams...>;
    using semantics = detail::concatenate_type_lists_t<
        record_semantics_t<typename Streams::record_type>...>;

    static constexpr std::size_t stream_count = sizeof...(Streams);

    static_assert(type_list_unique_v<semantics>,
                  "A semantic may occur only once across a VertexLayout's streams");
};

template<class Layout, class Requirements>
struct layout_satisfies
    : detail::layout_satisfies_list<Layout, detail::requirements_semantics_t<Requirements>> {};

template<class Layout, class Requirements>
inline constexpr bool layout_satisfies_v = layout_satisfies<Layout, Requirements>::value;

template<class Layout, class Requirements>
concept SatisfiesVertexInputs = layout_satisfies_v<Layout, Requirements>;

template<class Layout, class Tag>
struct layout_attribute;

template<class... Streams, class Tag>
struct layout_attribute<VertexLayout<Streams...>, Tag> {
private:
    using located = detail::locate_stream<Tag, 0, Streams...>;

public:
    using semantic_type = Tag;
    using stream_type = typename located::stream_type;
    using record_type = typename stream_type::record_type;
    using value_type = record_value_t<record_type, Tag>;
    using codec_type = record_codec_t<record_type, Tag>;

    static constexpr std::size_t binding = located::binding;
    static constexpr std::size_t offset = record_type::offset(Tag{});
    static constexpr std::size_t stride = record_type::stride;
    static constexpr u32 divisor = stream_type::divisor;
    static constexpr VertexFormat format = codec_type::format;
};

struct ResolvedVertexAttribute {
    std::string_view semantic_name;
    u32 location{};
    u32 binding{};
    std::size_t offset{};
    std::size_t stride{};
    u32 divisor{};
    VertexFormat format{};

    friend bool operator==(const ResolvedVertexAttribute&,
                           const ResolvedVertexAttribute&) = default;
};

struct ResolvedVertexInput {
    std::size_t stream_count{};
    std::vector<ResolvedVertexAttribute> attributes;

    friend bool operator==(const ResolvedVertexInput&, const ResolvedVertexInput&) = default;
};

namespace detail {

template<class Layout, class... Tags>
[[nodiscard]] ResolvedVertexInput resolve_vertex_input_list(TypeList<Tags...>)
{
    ResolvedVertexInput result;
    result.stream_count = Layout::stream_count;
    result.attributes.reserve(sizeof...(Tags));

    u32 location = 0;
    auto append = [&]<class Tag>() {
        using attribute = layout_attribute<Layout, Tag>;
        result.attributes.push_back({
            core::type_name<Tag>(),
            location++,
            static_cast<u32>(attribute::binding),
            attribute::offset,
            attribute::stride,
            attribute::divisor,
            attribute::format,
        });
    };
    (append.template operator()<Tags>(), ...);
    return result;
}

} // namespace detail

template<class Requirements, class Layout>
    requires SatisfiesVertexInputs<Layout, Requirements>
[[nodiscard]] ResolvedVertexInput resolve_vertex_input()
{
    return detail::resolve_vertex_input_list<Layout>(
        detail::requirements_semantics_t<Requirements>{});
}

[[nodiscard]] inline std::string dump_vertex_layout(const ResolvedVertexInput& input)
{
    std::ostringstream output;
    output << "vertex input: " << input.attributes.size() << " attributes, "
           << input.stream_count << " streams\n";
    for (const auto& attribute : input.attributes) {
        output << "  location " << attribute.location
               << ": " << attribute.semantic_name
               << ", binding " << attribute.binding
               << ", offset " << attribute.offset
               << ", stride " << attribute.stride
               << ", divisor " << attribute.divisor
               << ", " << detail::encoding_name(attribute.format.encoding)
               << 'x' << static_cast<unsigned>(attribute.format.component_count)
               << " (" << detail::interpretation_name(attribute.format.interpretation)
               << ")\n";
    }
    return output.str();
}

} // namespace vng::gfx
