#pragma once

#include <vng/core/types.hpp>
#include <vng/core/type_name.hpp>
#include <vng/gfx/record.hpp>
#include <vng/shader/interface.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace vng::shader {

template<class Tag>
struct Id {
    u32 value{std::numeric_limits<u32>::max()};

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return value != std::numeric_limits<u32>::max();
    }

    friend constexpr bool operator==(Id, Id) = default;
};

using TypeId = Id<struct TypeIdTag>;
using OperationId = Id<struct OperationIdTag>;
using RegionId = Id<struct RegionIdTag>;

struct ValueId {
    u32 value{std::numeric_limits<u32>::max()};
    u64 owner{};

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return value != std::numeric_limits<u32>::max() && owner != 0;
    }

    friend constexpr bool operator==(ValueId, ValueId) = default;
};

enum class TypeKind : std::uint8_t {
    poison,
    scalar,
    vector,
    matrix,
    record,
};

enum class ScalarKind : std::uint8_t {
    boolean,
    i32,
    u32,
    f32,
};

struct TypeMember {
    std::string semantic;
    TypeId type;

    friend bool operator==(const TypeMember&, const TypeMember&) = default;
};

struct TypeDescription {
    TypeKind kind{TypeKind::poison};
    ScalarKind scalar{ScalarKind::f32};
    u32 columns{1};
    u32 rows{1};
    std::string name;
    std::string canonical_key;
    std::vector<TypeMember> members;
};

class TypeTable {
public:
    TypeTable();

    [[nodiscard]] TypeId intern(TypeDescription description);
    [[nodiscard]] const TypeDescription& operator[](TypeId id) const;
    [[nodiscard]] std::size_t size() const noexcept { return types_.size(); }
    [[nodiscard]] const std::vector<TypeDescription>& descriptions() const noexcept { return types_; }
    [[nodiscard]] TypeId poison() const noexcept { return TypeId{0}; }

private:
    std::vector<TypeDescription> types_;
};

namespace detail {

template<class T>
[[nodiscard]] inline std::string type_name()
{
    return std::string(core::type_name<std::remove_cv_t<T>>());
}

template<class List>
struct type_list_size;

template<class... Ts>
struct type_list_size<gfx::TypeList<Ts...>> : std::integral_constant<std::size_t, sizeof...(Ts)> {};

template<class List, class Function>
constexpr void for_each_type(Function&& function);

template<class... Ts, class Function>
constexpr void for_each_type(gfx::TypeList<Ts...>, Function&& function)
{
    (function.template operator()<Ts>(), ...);
}

template<class List, class Function>
constexpr void for_each_type(Function&& function)
{
    for_each_type(List{}, static_cast<Function&&>(function));
}

template<class T>
struct record_traits;

template<class T>
    requires Interface<T>
struct record_traits<T> {
    using semantics = typename T::semantics;

    template<class Semantic>
    using value_type = typename Semantic::value_type;
};

template<class T>
    requires gfx::is_record_v<T>
struct record_traits<T> {
    using semantics = gfx::record_semantics_t<T>;

    template<class Semantic>
    using value_type = gfx::record_value_t<T, Semantic>;
};

template<class T>
concept RecordValue = Interface<T> || gfx::is_record_v<T>;

template<class List, class Semantic>
struct type_list_index;

template<class Semantic, class... Tail>
struct type_list_index<gfx::TypeList<Semantic, Tail...>, Semantic> : std::integral_constant<std::size_t, 0> {};

template<class Head, class... Tail, class Semantic>
struct type_list_index<gfx::TypeList<Head, Tail...>, Semantic>
    : std::integral_constant<std::size_t, 1 + type_list_index<gfx::TypeList<Tail...>, Semantic>::value> {};

template<class Semantic>
struct type_list_index<gfx::TypeList<>, Semantic> {
    static_assert(!std::is_same_v<Semantic, Semantic>, "semantic is not present in this record");
};

template<class List, class Semantic>
struct type_list_contains;

template<class Semantic, class... Ts>
struct type_list_contains<gfx::TypeList<Ts...>, Semantic>
    : std::bool_constant<(std::is_same_v<Ts, Semantic> || ...)> {};

template<class T>
struct is_matrix : std::false_type {};

template<std::size_t N>
struct is_matrix<Matrix<N>> : std::true_type {};

template<class T>
inline constexpr bool is_matrix_v = is_matrix<std::remove_cv_t<T>>::value;

template<class T>
struct matrix_traits;

template<std::size_t N>
struct matrix_traits<Matrix<N>> {
    using value_type = f32;
    static constexpr std::size_t columns = N;
    static constexpr std::size_t rows = N;
};

template<class T>
inline constexpr bool is_scalar_value_v =
    std::is_same_v<std::remove_cv_t<T>, bool> ||
    std::is_same_v<std::remove_cv_t<T>, i32> ||
    std::is_same_v<std::remove_cv_t<T>, u32> ||
    std::is_same_v<std::remove_cv_t<T>, f32>;

template<class T, bool = is_vector_v<T>>
struct is_vector_value : std::false_type {};

template<class T>
struct is_vector_value<T, true>
    : std::bool_constant<is_scalar_value_v<typename vector_traits<std::remove_cv_t<T>>::value_type>> {};

template<class T>
inline constexpr bool is_vector_value_v = is_vector_value<T>::value;

template<class T>
inline constexpr bool is_shader_value_v =
    is_scalar_value_v<T> || is_vector_value_v<T> || is_matrix_v<T> || RecordValue<std::remove_cv_t<T>>;

template<class T>
[[nodiscard]] TypeId register_type(TypeTable& table);

template<class T>
[[nodiscard]] TypeId register_scalar(TypeTable& table)
{
    TypeDescription description;
    description.kind = TypeKind::scalar;
    description.name = type_name<T>();

    if constexpr (std::is_same_v<T, bool>) {
        description.scalar = ScalarKind::boolean;
        description.canonical_key = "bool";
    } else if constexpr (std::is_same_v<T, i32>) {
        description.scalar = ScalarKind::i32;
        description.canonical_key = "i32";
    } else if constexpr (std::is_same_v<T, u32>) {
        description.scalar = ScalarKind::u32;
        description.canonical_key = "u32";
    } else {
        description.scalar = ScalarKind::f32;
        description.canonical_key = "f32";
    }
    return table.intern(std::move(description));
}

template<class T>
[[nodiscard]] TypeId register_type(TypeTable& table)
{
    using U = std::remove_cv_t<T>;
    static_assert(is_shader_value_v<U>, "T is not a supported shader value");

    if constexpr (is_scalar_value_v<U>) {
        return register_scalar<U>(table);
    } else if constexpr (is_vector_value_v<U>) {
        using Element = typename vector_traits<U>::value_type;
        const auto element = register_type<Element>(table);
        TypeDescription description;
        description.kind = TypeKind::vector;
        description.scalar = table[element].scalar;
        description.columns = static_cast<u32>(vector_traits<U>::component_count);
        description.name = type_name<U>();
        description.canonical_key = table[element].canonical_key + "x" + std::to_string(description.columns);
        return table.intern(std::move(description));
    } else if constexpr (is_matrix_v<U>) {
        const auto element = register_type<f32>(table);
        TypeDescription description;
        description.kind = TypeKind::matrix;
        description.scalar = table[element].scalar;
        description.columns = static_cast<u32>(matrix_traits<U>::columns);
        description.rows = static_cast<u32>(matrix_traits<U>::rows);
        description.name = type_name<U>();
        description.canonical_key = "mat" + std::to_string(description.columns) + "x" + std::to_string(description.rows);
        return table.intern(std::move(description));
    } else {
        TypeDescription description;
        description.kind = TypeKind::record;
        description.name = type_name<U>();
        description.canonical_key = "record{";

        using Semantics = typename record_traits<U>::semantics;
        for_each_type<Semantics>([&]<class Semantic> {
            using Value = typename record_traits<U>::template value_type<Semantic>;
            const auto member_type = register_type<Value>(table);
            auto semantic_name = type_name<Semantic>();
            description.canonical_key += semantic_name + ":" + table[member_type].canonical_key + ";";
            description.members.push_back(TypeMember{std::move(semantic_name), member_type});
        });
        description.canonical_key += "}";
        return table.intern(std::move(description));
    }
}

} // namespace detail

template<class T>
concept Value = detail::is_shader_value_v<std::remove_cv_t<T>>;

template<class Record, class Semantic>
concept RecordContains = detail::RecordValue<Record> &&
    detail::type_list_contains<typename detail::record_traits<Record>::semantics, Semantic>::value;

template<class Record, class Semantic>
    requires RecordContains<Record, Semantic>
using record_value_t = typename detail::record_traits<Record>::template value_type<Semantic>;

template<class Record, class Semantic>
    requires RecordContains<Record, Semantic>
inline constexpr std::size_t record_field_index_v =
    detail::type_list_index<typename detail::record_traits<Record>::semantics, Semantic>::value;

} // namespace vng::shader
