#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <vng/core/types.hpp>
#include <vng/gfx/record.hpp>
#include <vng/gfx/vertex_stream.hpp>

namespace vng::gfx {

template<class T>
concept RecordType = std::same_as<T, std::remove_cvref_t<T>> && is_record_v<T>;

struct MeshInfo final {
    std::string name;

    friend bool operator==(const MeshInfo&, const MeshInfo&) = default;
};

struct TriangleFace final {
    std::array<u32, 3> vertices{};

    constexpr TriangleFace() = default;
    constexpr TriangleFace(u32 first, u32 second, u32 third) noexcept
        : vertices{first, second, third}
    {}

    [[nodiscard]] constexpr u32& operator[](std::size_t index) noexcept
    {
        return vertices[index];
    }

    [[nodiscard]] constexpr const u32& operator[](std::size_t index) const noexcept
    {
        return vertices[index];
    }

    friend constexpr bool operator==(const TriangleFace&, const TriangleFace&) = default;
};

// A stable content fingerprint of the indexed triangle topology uploaded to a
// graphics backend. It includes the vertex count and the ordered face indices,
// so face order and winding are significant. Vertex attributes, MeshInfo, and
// explicit authored edges are intentionally excluded.
//
// This is a deterministic, non-cryptographic mismatch detector. Its 128-bit
// width makes accidental collisions vanishingly unlikely, but callers that
// need adversarially collision-proof identity must compare the topology itself.
struct MeshTopologyFingerprint final {
    u64 low{};
    u64 high{};

    friend constexpr bool operator==(
        const MeshTopologyFingerprint&,
        const MeshTopologyFingerprint&) = default;
};

struct Edge final {
    std::array<u32, 2> vertices{};

    constexpr Edge() = default;
    constexpr Edge(u32 first, u32 second) noexcept
        : vertices{first, second}
    {}

    [[nodiscard]] constexpr u32& operator[](std::size_t index) noexcept
    {
        return vertices[index];
    }

    [[nodiscard]] constexpr const u32& operator[](std::size_t index) const noexcept
    {
        return vertices[index];
    }

    friend constexpr bool operator==(const Edge&, const Edge&) = default;
};

enum class MeshDiagnosticCode {
    mismatched_stream_counts,
    too_many_vertices,
    face_index_out_of_bounds,
    edge_index_out_of_bounds,
};

struct MeshDiagnostic final {
    MeshDiagnosticCode code{};
    std::string message;
    std::size_t vertex_count{};
    std::optional<std::size_t> stream_index;
    std::optional<std::size_t> actual_stream_count;
    std::optional<std::size_t> primitive_index;
    std::optional<std::size_t> primitive_vertex;
    std::optional<u32> referenced_vertex;

    friend bool operator==(const MeshDiagnostic&, const MeshDiagnostic&) = default;
};

namespace detail {

[[nodiscard]] constexpr u64 avalanche_topology_word(u64 value) noexcept
{
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

class MeshTopologyFingerprintBuilder final {
public:
    constexpr void append(u64 value) noexcept
    {
        constexpr u64 ordinal_step = 0x9E3779B97F4A7C15ULL;
        const auto mixed = avalanche_topology_word(
            value + ordinal_ * ordinal_step);

        low_ = std::rotl(low_ ^ mixed, 27)
            * 0x3C79AC492BA7B653ULL + 0x1C69B3F74AC4AE35ULL;
        high_ = std::rotl(high_ + mixed * 0xD6E8FEB86659FD93ULL, 31)
            * 0x94D049BB133111EBULL + 0x2545F4914F6CDD1DULL;
        ++ordinal_;
    }

    [[nodiscard]] constexpr MeshTopologyFingerprint finish() const noexcept
    {
        const auto low = avalanche_topology_word(
            low_ ^ ordinal_ * 0x9E3779B97F4A7C15ULL);
        const auto high = avalanche_topology_word(
            high_ ^ std::rotl(low, 23) ^ ordinal_);
        return {.low = low, .high = high};
    }

private:
    u64 low_{0x243F6A8885A308D3ULL};
    u64 high_{0x13198A2E03707344ULL};
    u64 ordinal_{};
};

[[nodiscard]] inline MeshTopologyFingerprint mesh_topology_fingerprint(
    std::size_t vertex_count,
    const std::vector<TriangleFace>& faces) noexcept
{
    MeshTopologyFingerprintBuilder builder;
    // Domain-separate this representation from future line, patch, or polygon
    // topology fingerprints that may reuse the same mixing implementation.
    builder.append(0x564E475452493031ULL); // "VNGTRI01"
    builder.append(static_cast<u64>(vertex_count));
    builder.append(static_cast<u64>(faces.size()));
    for (const auto& face : faces) {
        builder.append(face.vertices[0]);
        builder.append(face.vertices[1]);
        builder.append(face.vertices[2]);
    }
    return builder.finish();
}

template<class... Lists>
struct concatenate_mesh_semantics;

template<>
struct concatenate_mesh_semantics<> {
    using type = TypeList<>;
};

template<class... Ts>
struct concatenate_mesh_semantics<TypeList<Ts...>> {
    using type = TypeList<Ts...>;
};

template<class... Left, class... Right, class... Tail>
struct concatenate_mesh_semantics<TypeList<Left...>, TypeList<Right...>, Tail...>
    : concatenate_mesh_semantics<TypeList<Left..., Right...>, Tail...> {};

template<class... Lists>
using concatenate_mesh_semantics_t = typename concatenate_mesh_semantics<Lists...>::type;

[[nodiscard]] inline std::expected<void, MeshDiagnostic> validate_mesh_vertex_count(
    std::size_t count)
{
    if (count > static_cast<std::size_t>(std::numeric_limits<u32>::max())) {
        return std::unexpected(MeshDiagnostic{
            .code = MeshDiagnosticCode::too_many_vertices,
            .message = "mesh vertex count exceeds the u32 vertex-count limit",
            .vertex_count = count,
            .stream_index = std::nullopt,
            .actual_stream_count = std::nullopt,
            .primitive_index = std::nullopt,
            .primitive_vertex = std::nullopt,
            .referenced_vertex = std::nullopt,
        });
    }
    return {};
}

} // namespace detail

template<class... RecordTypes>
    requires (sizeof...(RecordTypes) > 0) && (RecordType<RecordTypes> && ...)
class Mesh final {
public:
    using records = TypeList<RecordTypes...>;
    using semantics = detail::concatenate_mesh_semantics_t<
        record_semantics_t<RecordTypes>...>;
    using stream_tuple = std::tuple<VertexStream<RecordTypes>...>;
    using face_container = std::vector<TriangleFace>;
    using edge_container = std::vector<Edge>;
    using size_type = std::size_t;

    static constexpr size_type stream_count = sizeof...(RecordTypes);

    static_assert(type_list_unique_v<semantics>,
                  "A semantic may occur only once across a Mesh's vertex streams");

    explicit Mesh(size_type vertex_count = 0)
    {
        resize_vertices(vertex_count);
    }

    explicit Mesh(VertexStream<RecordTypes>... vertex_streams)
        : vertex_streams_(std::move(vertex_streams)...)
    {}

    [[nodiscard]] MeshInfo& info() noexcept { return info_; }
    [[nodiscard]] const MeshInfo& info() const noexcept { return info_; }

    [[nodiscard]] VertexStream<std::tuple_element_t<0, std::tuple<RecordTypes...>>>&
    vertices() noexcept
        requires (sizeof...(RecordTypes) == 1)
    {
        return std::get<0>(vertex_streams_);
    }

    [[nodiscard]] const VertexStream<std::tuple_element_t<0, std::tuple<RecordTypes...>>>&
    vertices() const noexcept
        requires (sizeof...(RecordTypes) == 1)
    {
        return std::get<0>(vertex_streams_);
    }

    template<RecordType Record>
        requires ((std::same_as<Record, RecordTypes>) || ...)
    [[nodiscard]] VertexStream<Record>& vertices() noexcept
    {
        return std::get<VertexStream<Record>>(vertex_streams_);
    }

    template<RecordType Record>
        requires ((std::same_as<Record, RecordTypes>) || ...)
    [[nodiscard]] const VertexStream<Record>& vertices() const noexcept
    {
        return std::get<VertexStream<Record>>(vertex_streams_);
    }

    template<SemanticType Tag>
    [[nodiscard]] static consteval bool has(Tag = {}) noexcept
    {
        return type_list_contains_v<semantics, std::remove_cv_t<Tag>>;
    }

    [[nodiscard]] size_type vertex_count() const noexcept
    {
        return std::get<0>(vertex_streams_).size();
    }

    [[nodiscard]] bool empty() const noexcept { return vertex_count() == 0; }

    void reserve_vertices(size_type count)
    {
        if (auto valid = validate_vertex_count(count); !valid) {
            throw std::length_error(valid.error().message);
        }
        for_each_vertex_stream([count](auto& stream) { stream.reserve(count); });
    }

    void resize_vertices(size_type count)
    {
        if (auto valid = validate_vertex_count(count); !valid) {
            throw std::length_error(valid.error().message);
        }
        // Reserve every stream before changing any size. If an allocation
        // fails, capacities may differ but the observable vertex counts stay
        // synchronized.
        reserve_vertices(count);
        for_each_vertex_stream([count](auto& stream) { stream.resize(count); });
    }

    template<class... Values>
        requires (sizeof...(Values) == sizeof...(RecordTypes))
                 && (std::same_as<std::remove_cvref_t<Values>, RecordTypes> && ...)
    void push_vertex(Values&&... values)
    {
        const auto count = vertex_count();
        const std::array<size_type, stream_count> counts{
            std::get<VertexStream<RecordTypes>>(vertex_streams_).size()...
        };
        for (const auto stream_size : counts) {
            if (stream_size != count) {
                throw std::logic_error(
                    "Mesh::push_vertex requires synchronized vertex streams");
            }
        }
        if (count >= static_cast<size_type>(std::numeric_limits<u32>::max())) {
            throw std::length_error(
                "Mesh::push_vertex would exceed the u32 vertex-count limit");
        }

        // Stage values before reserve so callers may safely pass records that
        // currently live in one of this mesh's streams.
        auto staged = std::tuple<RecordTypes...>{
            std::forward<Values>(values)...};
        reserve_vertices(count + 1);
        std::apply(
            [&](auto&... records) {
                (std::get<VertexStream<RecordTypes>>(vertex_streams_)
                     .push_back(std::move(records)), ...);
            },
            staged);
    }

    template<class Function>
    constexpr void for_each_vertex_stream(Function&& function)
    {
        std::apply(
            [&](auto&... streams) { (static_cast<void>(function(streams)), ...); },
            vertex_streams_);
    }

    template<class Function>
    constexpr void for_each_vertex_stream(Function&& function) const
    {
        std::apply(
            [&](const auto&... streams) { (static_cast<void>(function(streams)), ...); },
            vertex_streams_);
    }

    [[nodiscard]] face_container& faces() noexcept { return faces_; }
    [[nodiscard]] const face_container& faces() const noexcept { return faces_; }
    [[nodiscard]] size_type face_count() const noexcept { return faces_.size(); }

    [[nodiscard]] MeshTopologyFingerprint topology_fingerprint() const noexcept
    {
        return detail::mesh_topology_fingerprint(vertex_count(), faces_);
    }

    void add_face(TriangleFace face) { faces_.push_back(face); }

    void add_face(u32 first, u32 second, u32 third)
    {
        add_face(TriangleFace{first, second, third});
    }

    [[nodiscard]] bool has_explicit_edges() const noexcept
    {
        return explicit_edges_.has_value();
    }

    [[nodiscard]] size_type explicit_edge_count() const noexcept
    {
        return explicit_edges_ ? explicit_edges_->size() : 0;
    }

    [[nodiscard]] std::optional<edge_container>& explicit_edges() noexcept
    {
        return explicit_edges_;
    }

    [[nodiscard]] const std::optional<edge_container>& explicit_edges() const noexcept
    {
        return explicit_edges_;
    }

    void add_edge(Edge edge)
    {
        if (!explicit_edges_) {
            explicit_edges_.emplace();
        }
        explicit_edges_->push_back(edge);
    }

    void add_edge(u32 first, u32 second)
    {
        add_edge(Edge{first, second});
    }

    void set_explicit_edges(edge_container edges = {})
    {
        explicit_edges_ = std::move(edges);
    }

    void omit_explicit_edges() noexcept
    {
        explicit_edges_.reset();
    }

    [[nodiscard]] static std::expected<void, MeshDiagnostic> validate_vertex_count(
        size_type count)
    {
        return detail::validate_mesh_vertex_count(count);
    }

    [[nodiscard]] std::expected<void, MeshDiagnostic> validate() const
    {
        const auto count = vertex_count();
        const std::array<size_type, stream_count> counts{
            std::get<VertexStream<RecordTypes>>(vertex_streams_).size()...
        };
        for (size_type index = 1; index < counts.size(); ++index) {
            if (counts[index] != count) {
                return std::unexpected(MeshDiagnostic{
                    .code = MeshDiagnosticCode::mismatched_stream_counts,
                    .message = "mesh vertex stream " + std::to_string(index)
                        + " contains " + std::to_string(counts[index])
                        + " records; expected " + std::to_string(count),
                    .vertex_count = count,
                    .stream_index = index,
                    .actual_stream_count = counts[index],
                    .primitive_index = std::nullopt,
                    .primitive_vertex = std::nullopt,
                    .referenced_vertex = std::nullopt,
                });
            }
        }

        if (auto valid_count = validate_vertex_count(count); !valid_count) {
            return valid_count;
        }

        for (size_type face_index = 0; face_index < faces_.size(); ++face_index) {
            for (size_type corner = 0; corner < 3; ++corner) {
                const auto referenced = faces_[face_index][corner];
                if (static_cast<size_type>(referenced) >= count) {
                    return std::unexpected(MeshDiagnostic{
                        .code = MeshDiagnosticCode::face_index_out_of_bounds,
                        .message = "mesh face " + std::to_string(face_index)
                            + " corner " + std::to_string(corner)
                            + " references vertex " + std::to_string(referenced)
                            + ", but the mesh contains " + std::to_string(count)
                            + " vertices",
                        .vertex_count = count,
                        .stream_index = std::nullopt,
                        .actual_stream_count = std::nullopt,
                        .primitive_index = face_index,
                        .primitive_vertex = corner,
                        .referenced_vertex = referenced,
                    });
                }
            }
        }

        if (explicit_edges_) {
            for (size_type edge_index = 0; edge_index < explicit_edges_->size();
                 ++edge_index) {
                for (size_type endpoint = 0; endpoint < 2; ++endpoint) {
                    const auto referenced = (*explicit_edges_)[edge_index][endpoint];
                    if (static_cast<size_type>(referenced) >= count) {
                        return std::unexpected(MeshDiagnostic{
                            .code = MeshDiagnosticCode::edge_index_out_of_bounds,
                            .message = "mesh edge " + std::to_string(edge_index)
                                + " endpoint " + std::to_string(endpoint)
                                + " references vertex " + std::to_string(referenced)
                                + ", but the mesh contains " + std::to_string(count)
                                + " vertices",
                            .vertex_count = count,
                            .stream_index = std::nullopt,
                            .actual_stream_count = std::nullopt,
                            .primitive_index = edge_index,
                            .primitive_vertex = endpoint,
                            .referenced_vertex = referenced,
                        });
                    }
                }
            }
        }
        return {};
    }

private:
    MeshInfo info_;
    stream_tuple vertex_streams_;
    face_container faces_;
    std::optional<edge_container> explicit_edges_;
};

template<class... RecordTypes>
Mesh(VertexStream<RecordTypes>...) -> Mesh<RecordTypes...>;

} // namespace vng::gfx
