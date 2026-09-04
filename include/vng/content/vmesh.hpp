#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <vng/content/diagnostic.hpp>
#include <vng/core/types.hpp>
#include <vng/gfx/mesh.hpp>

namespace vng::content::vmesh {

enum class ScalarType {
    Float32,
    Int32,
    UInt32,
};

struct FieldType final {
    ScalarType scalar{ScalarType::Float32};
    u8 components{1};

    friend constexpr bool operator==(const FieldType&, const FieldType&) = default;
};

using FieldValues = std::variant<
    std::vector<f32>,
    std::vector<i32>,
    std::vector<u32>>;

// Values are columnar. A field with C components in a document with N vertices
// contains N*C flattened scalar values, grouped by vertex.
struct VertexField final {
    std::string name;
    FieldType type;
    FieldValues values;

    friend bool operator==(const VertexField&, const VertexField&) = default;
};

struct Document final {
    std::map<std::string, std::string, std::less<>> metadata;
    std::size_t vertex_count{};
    std::vector<VertexField> vertex_fields;
    std::vector<gfx::TriangleFace> faces;
    std::optional<std::vector<gfx::Edge>> edges;

    friend bool operator==(const Document&, const Document&) = default;
};

struct Limits final {
    std::size_t max_source_bytes{256U * 1024U * 1024U};
    std::size_t max_decoded_bytes{256U * 1024U * 1024U};
    std::size_t max_metadata_entries{1024};
    std::size_t max_metadata_key_bytes{256};
    std::size_t max_metadata_value_bytes{1024U * 1024U};
    std::size_t max_vertex_fields{64};
    std::size_t max_vertices{16U * 1024U * 1024U};
    std::size_t max_faces{32U * 1024U * 1024U};
    std::size_t max_edges{32U * 1024U * 1024U};
    std::size_t max_scalar_values{256U * 1024U * 1024U};
};

struct ReadOptions final {
    Limits limits{};
};

struct WriteOptions final {
    Limits limits{};
};

// Stable names are the serialization ABI of .vmesh. They are intentionally
// independent of C++ semantic type spellings.
[[nodiscard]] bool is_valid_name(std::string_view name) noexcept;

// Structural validation has no configurable resource ceiling. The overload
// taking Limits additionally applies the same budgets used by parsing/writing.
[[nodiscard]] Result<void> validate(const Document& document);
[[nodiscard]] Result<void> validate(
    const Document& document,
    const Limits& limits);

[[nodiscard]] Result<Document> parse_vmesh(
    std::string_view source,
    const ReadOptions& options = {});

[[nodiscard]] Result<Document> read_vmesh(
    const std::filesystem::path& path,
    const ReadOptions& options = {});

[[nodiscard]] Result<std::string> write_vmesh(
    const Document& document,
    const WriteOptions& options = {});

[[nodiscard]] Result<void> write_vmesh(
    const std::filesystem::path& path,
    const Document& document,
    const WriteOptions& options = {});

} // namespace vng::content::vmesh
