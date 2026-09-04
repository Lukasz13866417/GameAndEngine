#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <vng/content/vmesh.hpp>
#include <vng/core/type_name.hpp>
#include <vng/gfx/mesh.hpp>
#include <vng/gfx/semantic.hpp>

namespace vng::content::vmesh {

template<gfx::SemanticType Semantic>
struct FieldMapping final {
    using semantic_type = std::remove_cv_t<Semantic>;

    std::string name;
};

template<gfx::SemanticType Semantic>
[[nodiscard]] FieldMapping<std::remove_cv_t<Semantic>> map(
    std::string_view name,
    Semantic)
{
    return FieldMapping<std::remove_cv_t<Semantic>>{std::string{name}};
}

namespace schema_detail {

template<class T>
struct is_field_mapping : std::false_type {};

template<gfx::SemanticType Semantic>
struct is_field_mapping<FieldMapping<Semantic>> : std::true_type {};

template<class T>
inline constexpr bool is_field_mapping_v =
    is_field_mapping<std::remove_cvref_t<T>>::value;

template<class Scalar>
inline constexpr bool supported_scalar_v =
    std::same_as<std::remove_cv_t<Scalar>, f32>
    || std::same_as<std::remove_cv_t<Scalar>, i32>
    || std::same_as<std::remove_cv_t<Scalar>, u32>;

template<class Scalar>
[[nodiscard]] consteval ScalarType scalar_type_for()
{
    if constexpr (std::same_as<std::remove_cv_t<Scalar>, i32>) {
        return ScalarType::Int32;
    } else if constexpr (std::same_as<std::remove_cv_t<Scalar>, u32>) {
        return ScalarType::UInt32;
    } else {
        return ScalarType::Float32;
    }
}

template<class Value>
struct logical_type_traits_impl {
    using scalar_type = std::remove_cv_t<Value>;

    static constexpr bool supported = supported_scalar_v<scalar_type>;
    static constexpr std::size_t components = 1;
    static constexpr ScalarType scalar = scalar_type_for<scalar_type>();
};

template<class Scalar, std::size_t Components>
struct logical_type_traits_impl<Vector<Scalar, Components>> {
    using scalar_type = std::remove_cv_t<Scalar>;

    static constexpr bool supported =
        supported_scalar_v<scalar_type> && Components >= 2 && Components <= 4;
    static constexpr std::size_t components = Components;
    static constexpr ScalarType scalar = scalar_type_for<scalar_type>();
};

template<class Value>
using logical_type_traits = logical_type_traits_impl<std::remove_cv_t<Value>>;

template<class Mapping>
using mapping_semantic_t = typename std::remove_cvref_t<Mapping>::semantic_type;

template<class Mapping>
using mapping_value_t = gfx::semantic_value_t<mapping_semantic_t<Mapping>>;

template<class Mapping>
inline constexpr bool mapping_value_supported_v =
    logical_type_traits<mapping_value_t<Mapping>>::supported;

template<class Semantic, class... Semantics>
[[nodiscard]] consteval std::size_t semantic_index(
    gfx::TypeList<Semantics...>) noexcept
{
    constexpr std::array matches{
        std::same_as<std::remove_cv_t<Semantic>, Semantics>...
    };
    for (std::size_t index = 0; index < matches.size(); ++index) {
        if (matches[index]) {
            return index;
        }
    }
    return matches.size();
}

} // namespace schema_detail

template<gfx::RecordType Record, class... Mappings>
    requires (sizeof...(Mappings) > 0)
             && (schema_detail::is_field_mapping_v<Mappings> && ...)
class Schema final {
public:
    using record_type = std::remove_cv_t<Record>;
    using mapping_types = gfx::TypeList<std::remove_cv_t<Mappings>...>;
    using mapping_tuple = std::tuple<std::remove_cv_t<Mappings>...>;

    static_assert(sizeof...(Mappings) == record_type::field_count,
                  "A vmesh schema must map every Record semantic exactly once");
    static_assert(
        (gfx::record_has_v<record_type, schema_detail::mapping_semantic_t<Mappings>> && ...),
        "A vmesh schema field must name a semantic in its Record");
    static_assert(
        gfx::type_list_unique_v<gfx::TypeList<
            schema_detail::mapping_semantic_t<Mappings>...>>,
        "A vmesh schema must map each Record semantic exactly once");
    static_assert(
        (schema_detail::mapping_value_supported_v<Mappings> && ...),
        "A vmesh schema supports only f32, i32, u32, and their 2-4 component vectors");

    explicit Schema(std::remove_cv_t<Mappings>... mappings)
        : mappings_(std::move(mappings)...)
    {}

    [[nodiscard]] const mapping_tuple& mappings() const noexcept
    {
        return mappings_;
    }

    [[nodiscard]] static constexpr std::size_t mapping_count() noexcept
    {
        return sizeof...(Mappings);
    }

    [[nodiscard]] Result<void> validate() const
    {
        return {};
    }

    template<class Function>
    constexpr void for_each_mapping(Function&& function) const
    {
        std::apply(
            [&](const auto&... mappings) {
                (static_cast<void>(function(mappings)), ...);
            },
            mappings_);
    }

private:
    mapping_tuple mappings_;
};

template<gfx::RecordType Record,
         class Semantics = gfx::record_semantics_t<Record>>
class SchemaBuilder;

template<gfx::RecordType Record, gfx::SemanticType... Semantics>
class SchemaBuilder<Record, gfx::TypeList<Semantics...>> final {
public:
    using record_type = std::remove_cv_t<Record>;
    using mapping_types = gfx::TypeList<FieldMapping<Semantics>...>;
    using mapping_tuple =
        std::tuple<std::optional<FieldMapping<Semantics>>...>;

    static_assert(
        (schema_detail::mapping_value_supported_v<FieldMapping<Semantics>>
         && ...),
        "A vmesh schema supports only f32, i32, u32, and their 2-4 component vectors");

    template<gfx::SemanticType Semantic>
    SchemaBuilder& map(std::string_view name, Semantic)
    {
        using semantic_type = std::remove_cv_t<Semantic>;
        if constexpr (!gfx::record_has_v<record_type, semantic_type>) {
            static_assert(
                gfx::record_has_v<record_type, semantic_type>,
                "A vmesh schema field must name a semantic in its Record");
        } else {
            constexpr auto index = schema_detail::semantic_index<semantic_type>(
                gfx::record_semantics_t<record_type>{});
            auto& mapping = std::get<index>(mappings_);
            if (mapping) {
                if (!construction_error_) {
                    construction_error_ = Diagnostic{
                        .code = ErrorCode::duplicate_field,
                        .message = "vmesh schema maps semantic '"
                            + std::string{core::type_name<semantic_type>()}
                            + "' more than once",
                        .location = std::nullopt,
                        .path = {},
                        .notes = {
                            "first file field: " + mapping->name,
                            "duplicate file field: " + std::string{name},
                        },
                    };
                }
                return *this;
            }

            mapping.emplace(FieldMapping<semantic_type>{std::string{name}});
            order_[mapped_count_++] = index;
        }
        return *this;
    }

    [[nodiscard]] std::size_t mapping_count() const noexcept
    {
        return mapped_count_;
    }

    [[nodiscard]] Result<void> validate() const
    {
        if (construction_error_) {
            return std::unexpected(*construction_error_);
        }
        if (mapped_count_ == record_type::field_count) {
            return {};
        }

        Diagnostic diagnostic{
            .code = ErrorCode::invalid_document,
            .message = "vmesh schema maps " + std::to_string(mapped_count_)
                + " of " + std::to_string(record_type::field_count)
                + " Record semantics",
            .location = std::nullopt,
            .path = {},
            .notes = {},
        };
        auto add_missing_note = [&]<class Semantic>() {
            constexpr auto index = schema_detail::semantic_index<Semantic>(
                gfx::record_semantics_t<record_type>{});
            if (!std::get<index>(mappings_)) {
                static_cast<void>(diagnostic.note(
                    "missing semantic: "
                    + std::string{core::type_name<Semantic>()}));
            }
        };
        (add_missing_note.template operator()<Semantics>(), ...);
        return std::unexpected(std::move(diagnostic));
    }

    template<class Function>
    constexpr void for_each_mapping(Function&& function) const
    {
        auto&& callable = function;
        for (std::size_t position = 0; position < mapped_count_; ++position) {
            const auto wanted = order_[position];
            [&]<std::size_t... Indices>(std::index_sequence<Indices...>) {
                (static_cast<void>(
                     wanted == Indices
                         ? (static_cast<void>(
                                callable(*std::get<Indices>(mappings_))),
                            0)
                         : 0),
                 ...);
            }(std::index_sequence_for<Semantics...>{});
        }
    }

private:
    mapping_tuple mappings_;
    std::array<std::size_t, sizeof...(Semantics)> order_{};
    std::size_t mapped_count_{};
    std::optional<Diagnostic> construction_error_;
};

template<gfx::RecordType Record, class... Mappings>
    requires (sizeof...(Mappings) > 0)
             && (schema_detail::is_field_mapping_v<Mappings> && ...)
[[nodiscard]] auto schema(Mappings... mappings)
{
    return Schema<std::remove_cv_t<Record>, std::remove_cv_t<Mappings>...>{
        std::move(mappings)...};
}

template<gfx::RecordType Record>
[[nodiscard]] auto schema()
{
    return SchemaBuilder<std::remove_cv_t<Record>>{};
}

namespace schema_detail {

template<class T>
struct is_schema : std::false_type {};

template<gfx::RecordType Record, class... Mappings>
struct is_schema<Schema<Record, Mappings...>> : std::true_type {};

template<gfx::RecordType Record, class Semantics>
struct is_schema<SchemaBuilder<Record, Semantics>> : std::true_type {};

template<class T>
inline constexpr bool is_schema_v = is_schema<std::remove_cvref_t<T>>::value;

template<class SchemaType>
using schema_record_t = typename std::remove_cvref_t<SchemaType>::record_type;

template<class SchemaType, class Function>
constexpr void for_each_mapping(const SchemaType& schema_value, Function&& function)
{
    schema_value.for_each_mapping(std::forward<Function>(function));
}

template<class... Schemas>
[[nodiscard]] Result<void> validate_schema_definitions(
    const Schemas&... schemas)
{
    std::optional<Diagnostic> error;
    auto validate_one = [&](const auto& schema_value) {
        if (error) {
            return;
        }
        auto valid = schema_value.validate();
        if (!valid) {
            error = std::move(valid.error());
        }
    };
    (validate_one(schemas), ...);
    if (error) {
        return std::unexpected(std::move(*error));
    }
    return {};
}

template<class... Schemas>
[[nodiscard]] Result<void> validate_schema_names(const Schemas&... schemas)
{
    std::unordered_set<std::string_view> names;
    std::optional<Diagnostic> error;

    auto validate = [&](const auto& mapping) {
        if (error) {
            return;
        }
        if (!is_valid_name(mapping.name)) {
            error = Diagnostic{
                .code = ErrorCode::invalid_document,
                .message = "vmesh schema field name '" + mapping.name
                    + "' is not a stable slash-separated identifier",
                .location = std::nullopt,
                .path = {},
                .notes = {},
            };
            return;
        }
        if (!names.emplace(std::string_view{mapping.name}).second) {
            error = Diagnostic{
                .code = ErrorCode::duplicate_field,
                .message = "vmesh schema maps the file field name '"
                    + mapping.name + "' more than once",
                .location = std::nullopt,
                .path = {},
                .notes = {},
            };
        }
    };

    (for_each_mapping(schemas, validate), ...);
    if (error) {
        return std::unexpected(std::move(*error));
    }
    return {};
}

using DocumentFieldIndex =
    std::unordered_map<std::string_view, const VertexField*>;

[[nodiscard]] inline Result<DocumentFieldIndex> index_document_fields(
    const Document& document)
{
    DocumentFieldIndex fields;
    fields.reserve(document.vertex_fields.size());
    for (const auto& field_value : document.vertex_fields) {
        if (field_value.name.empty()) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::invalid_document,
                .message = "vmesh document contains an empty vertex field name",
                .location = std::nullopt,
                .path = {},
                .notes = {},
            });
        }
        if (!fields.emplace(std::string_view{field_value.name}, &field_value).second) {
            return std::unexpected(Diagnostic{
                .code = ErrorCode::duplicate_field,
                .message = "vmesh document contains duplicate vertex field '"
                    + field_value.name + "'",
                .location = std::nullopt,
                .path = {},
                .notes = {},
            });
        }
    }
    return fields;
}

template<class Mapping>
[[nodiscard]] constexpr FieldType mapped_field_type() noexcept
{
    using traits = logical_type_traits<mapping_value_t<Mapping>>;
    static_assert(traits::supported);
    return FieldType{
        .scalar = traits::scalar,
        .components = static_cast<u8>(traits::components),
    };
}

[[nodiscard]] inline Result<std::size_t> scalar_value_count(
    std::size_t vertex_count,
    std::size_t component_count,
    std::string_view field_name)
{
    if (component_count == 0
        || vertex_count > std::numeric_limits<std::size_t>::max() / component_count) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::count_mismatch,
            .message = "vmesh field '" + std::string{field_name}
                + "' has an unrepresentable scalar value count",
            .location = std::nullopt,
            .path = {},
            .notes = {},
        });
    }
    return vertex_count * component_count;
}

template<class Mapping>
[[nodiscard]] Result<void> validate_mapped_document_field(
    const Document& document,
    const DocumentFieldIndex& fields,
    const Mapping& mapping)
{
    const auto found = fields.find(mapping.name);
    if (found == fields.end()) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_document,
            .message = "vmesh document is missing schema field '"
                + mapping.name + "'",
            .location = std::nullopt,
            .path = {},
            .notes = {},
        });
    }

    using traits = logical_type_traits<mapping_value_t<Mapping>>;
    using scalar_type = typename traits::scalar_type;
    const auto* document_field = found->second;
    const auto expected_type = mapped_field_type<Mapping>();
    if (document_field->type != expected_type
        || !std::holds_alternative<std::vector<scalar_type>>(
            document_field->values)) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::invalid_field_type,
            .message = "vmesh field '" + mapping.name
                + "' is incompatible with its schema semantic",
            .location = std::nullopt,
            .path = {},
            .notes = {},
        });
    }

    auto expected_count = scalar_value_count(
        document.vertex_count, traits::components, mapping.name);
    if (!expected_count) {
        return std::unexpected(std::move(expected_count.error()));
    }
    const auto& values = std::get<std::vector<scalar_type>>(
        document_field->values);
    if (values.size() != *expected_count) {
        return std::unexpected(Diagnostic{
            .code = ErrorCode::count_mismatch,
            .message = "vmesh field '" + mapping.name + "' contains "
                + std::to_string(values.size()) + " scalar values; expected "
                + std::to_string(*expected_count),
            .location = std::nullopt,
            .path = {},
            .notes = {},
        });
    }
    return {};
}

template<class... Schemas>
[[nodiscard]] Result<void> validate_mapped_document_fields(
    const Document& document,
    const DocumentFieldIndex& fields,
    const Schemas&... schemas)
{
    std::optional<Diagnostic> error;
    auto validate = [&](const auto& mapping) {
        if (error) {
            return;
        }
        auto valid = validate_mapped_document_field(document, fields, mapping);
        if (!valid) {
            error = std::move(valid.error());
        }
    };
    (for_each_mapping(schemas, validate), ...);
    if (error) {
        return std::unexpected(std::move(*error));
    }
    return {};
}

template<class... Schemas>
[[nodiscard]] Result<void> validate_encoded_value_counts(
    std::size_t vertex_count,
    const Schemas&... schemas)
{
    std::optional<Diagnostic> error;
    auto validate = [&](const auto& mapping) {
        if (error) {
            return;
        }
        using mapping_type = std::remove_cvref_t<decltype(mapping)>;
        using traits = logical_type_traits<mapping_value_t<mapping_type>>;
        auto count = scalar_value_count(
            vertex_count, traits::components, mapping.name);
        if (!count) {
            error = std::move(count.error());
        }
    };
    (for_each_mapping(schemas, validate), ...);
    if (error) {
        return std::unexpected(std::move(*error));
    }
    return {};
}

template<class Record, class Mapping>
void decode_mapping(
    gfx::VertexStream<Record>& records,
    const DocumentFieldIndex& fields,
    const Mapping& mapping)
{
    using semantic_type = mapping_semantic_t<Mapping>;
    using value_type = mapping_value_t<Mapping>;
    using traits = logical_type_traits<value_type>;
    using scalar_type = typename traits::scalar_type;

    const auto* document_field = fields.at(mapping.name);
    const auto& values = std::get<std::vector<scalar_type>>(
        document_field->values);
    for (std::size_t vertex = 0; vertex < records.size(); ++vertex) {
        if constexpr (traits::components == 1) {
            records[vertex].set(semantic_type{}, values[vertex]);
        } else {
            value_type value{};
            const auto base = vertex * traits::components;
            for (std::size_t component = 0; component < traits::components;
                 ++component) {
                value[component] = values[base + component];
            }
            records[vertex].set(semantic_type{}, value);
        }
    }
}

template<class Mesh, class SchemaType>
void decode_schema(
    Mesh& mesh,
    const DocumentFieldIndex& fields,
    const SchemaType& schema_value)
{
    using record_type = schema_record_t<SchemaType>;
    auto& records = mesh.template vertices<record_type>();
    for_each_mapping(schema_value, [&](const auto& mapping) {
        decode_mapping(records, fields, mapping);
    });
}

[[nodiscard]] inline Diagnostic from_mesh_diagnostic(
    const gfx::MeshDiagnostic& diagnostic)
{
    const bool index_error =
        diagnostic.code == gfx::MeshDiagnosticCode::face_index_out_of_bounds
        || diagnostic.code == gfx::MeshDiagnosticCode::edge_index_out_of_bounds;
    return Diagnostic{
        .code = index_error ? ErrorCode::index_out_of_range
                            : ErrorCode::invalid_document,
        .message = diagnostic.message,
        .location = std::nullopt,
        .path = {},
        .notes = {},
    };
}

template<class Record, class Mapping>
void encode_mapping(
    Document& document,
    const gfx::VertexStream<Record>& records,
    const Mapping& mapping)
{
    using semantic_type = mapping_semantic_t<Mapping>;
    using value_type = mapping_value_t<Mapping>;
    using traits = logical_type_traits<value_type>;
    using scalar_type = typename traits::scalar_type;

    std::vector<scalar_type> values;
    values.reserve(records.size() * traits::components);
    for (const auto& record : records) {
        const auto value = record.get(semantic_type{});
        if constexpr (traits::components == 1) {
            values.push_back(value);
        } else {
            for (std::size_t component = 0; component < traits::components;
                 ++component) {
                values.push_back(value[component]);
            }
        }
    }

    document.vertex_fields.push_back(VertexField{
        .name = mapping.name,
        .type = mapped_field_type<Mapping>(),
        .values = FieldValues{std::move(values)},
    });
}

template<class Mesh, class SchemaType>
void encode_schema(
    Document& document,
    const Mesh& mesh,
    const SchemaType& schema_value)
{
    using record_type = schema_record_t<SchemaType>;
    const auto& records = mesh.template vertices<record_type>();
    for_each_mapping(schema_value, [&](const auto& mapping) {
        encode_mapping(document, records, mapping);
    });
}

} // namespace schema_detail

template<class... Schemas>
    requires (sizeof...(Schemas) > 0)
             && (schema_detail::is_schema_v<Schemas> && ...)
[[nodiscard]] auto decode(
    const Document& document,
    const Schemas&... schemas)
    -> Result<gfx::Mesh<schema_detail::schema_record_t<Schemas>...>>
{
    using mesh_type = gfx::Mesh<schema_detail::schema_record_t<Schemas>...>;

    if (auto valid_schemas =
            schema_detail::validate_schema_definitions(schemas...);
        !valid_schemas) {
        return std::unexpected(std::move(valid_schemas.error()));
    }
    if (auto valid_document = validate(document); !valid_document) {
        return std::unexpected(std::move(valid_document.error()));
    }
    if (auto valid_names = schema_detail::validate_schema_names(schemas...);
        !valid_names) {
        return std::unexpected(std::move(valid_names.error()));
    }
    auto fields = schema_detail::index_document_fields(document);
    if (!fields) {
        return std::unexpected(std::move(fields.error()));
    }
    if (auto valid_fields = schema_detail::validate_mapped_document_fields(
            document, *fields, schemas...);
        !valid_fields) {
        return std::unexpected(std::move(valid_fields.error()));
    }
    if (auto valid_count = mesh_type::validate_vertex_count(document.vertex_count);
        !valid_count) {
        return std::unexpected(
            schema_detail::from_mesh_diagnostic(valid_count.error()));
    }

    mesh_type mesh{document.vertex_count};
    mesh.faces() = document.faces;
    mesh.explicit_edges() = document.edges;
    if (const auto name = document.metadata.find("name");
        name != document.metadata.end()) {
        mesh.info().name = name->second;
    }

    (schema_detail::decode_schema(mesh, *fields, schemas), ...);
    if (auto valid_mesh = mesh.validate(); !valid_mesh) {
        return std::unexpected(
            schema_detail::from_mesh_diagnostic(valid_mesh.error()));
    }
    return mesh;
}

template<class... Records, class... Schemas>
    requires (sizeof...(Records) > 0)
             && (sizeof...(Records) == sizeof...(Schemas))
             && (schema_detail::is_schema_v<Schemas> && ...)
             && std::same_as<
                 std::tuple<std::remove_cv_t<Records>...>,
                 std::tuple<schema_detail::schema_record_t<Schemas>...>>
[[nodiscard]] Result<Document> encode(
    const gfx::Mesh<Records...>& mesh,
    const Schemas&... schemas)
{
    if (auto valid_schemas =
            schema_detail::validate_schema_definitions(schemas...);
        !valid_schemas) {
        return std::unexpected(std::move(valid_schemas.error()));
    }
    if (auto valid_names = schema_detail::validate_schema_names(schemas...);
        !valid_names) {
        return std::unexpected(std::move(valid_names.error()));
    }
    if (auto valid_mesh = mesh.validate(); !valid_mesh) {
        return std::unexpected(
            schema_detail::from_mesh_diagnostic(valid_mesh.error()));
    }
    if (auto valid_counts = schema_detail::validate_encoded_value_counts(
            mesh.vertex_count(), schemas...);
        !valid_counts) {
        return std::unexpected(std::move(valid_counts.error()));
    }

    Document document;
    if (!mesh.info().name.empty()) {
        document.metadata.emplace("name", mesh.info().name);
    }
    document.vertex_count = mesh.vertex_count();
    document.faces = mesh.faces();
    document.edges = mesh.explicit_edges();
    document.vertex_fields.reserve((schemas.mapping_count() + ...));
    (schema_detail::encode_schema(document, mesh, schemas), ...);
    if (auto valid_document = validate(document); !valid_document) {
        return std::unexpected(std::move(valid_document.error()));
    }
    return document;
}

template<class... Schemas>
    requires (sizeof...(Schemas) > 0)
             && (schema_detail::is_schema_v<Schemas> && ...)
[[nodiscard]] auto load(
    const std::filesystem::path& path,
    const ReadOptions& options,
    const Schemas&... schemas)
    -> Result<gfx::Mesh<schema_detail::schema_record_t<Schemas>...>>
{
    auto document = read_vmesh(path, options);
    if (!document) {
        return std::unexpected(std::move(document.error()));
    }
    auto mesh = decode(*document, schemas...);
    if (!mesh) {
        mesh.error().path = path;
    }
    return mesh;
}

template<class... Schemas>
    requires (sizeof...(Schemas) > 0)
             && (schema_detail::is_schema_v<Schemas> && ...)
[[nodiscard]] auto load(
    const std::filesystem::path& path,
    const Schemas&... schemas)
    -> Result<gfx::Mesh<schema_detail::schema_record_t<Schemas>...>>
{
    return load(path, ReadOptions{}, schemas...);
}

template<class... Records, class... Schemas>
    requires (sizeof...(Records) > 0)
             && (sizeof...(Records) == sizeof...(Schemas))
             && (schema_detail::is_schema_v<Schemas> && ...)
             && std::same_as<
                 std::tuple<std::remove_cv_t<Records>...>,
                 std::tuple<schema_detail::schema_record_t<Schemas>...>>
[[nodiscard]] Result<void> save(
    const std::filesystem::path& path,
    const gfx::Mesh<Records...>& mesh,
    const WriteOptions& options,
    const Schemas&... schemas)
{
    auto document = encode(mesh, schemas...);
    if (!document) {
        document.error().path = path;
        return std::unexpected(std::move(document.error()));
    }
    return write_vmesh(path, *document, options);
}

template<class... Records, class... Schemas>
    requires (sizeof...(Records) > 0)
             && (sizeof...(Records) == sizeof...(Schemas))
             && (schema_detail::is_schema_v<Schemas> && ...)
             && std::same_as<
                 std::tuple<std::remove_cv_t<Records>...>,
                 std::tuple<schema_detail::schema_record_t<Schemas>...>>
[[nodiscard]] Result<void> save(
    const std::filesystem::path& path,
    const gfx::Mesh<Records...>& mesh,
    const Schemas&... schemas)
{
    return save(path, mesh, WriteOptions{}, schemas...);
}

} // namespace vng::content::vmesh
