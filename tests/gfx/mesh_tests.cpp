#include <vng/gfx/gfx.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec3> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};
struct Normal : vng::gfx::Semantic<vng::Vec3> {};
struct Missing : vng::gfx::Semantic<vng::f32> {};

using Geometry = vng::gfx::Record<Position, Normal>;
using Surface = vng::gfx::Record<vng::gfx::as<Color, vng::gfx::unorm8x4>>;

} // namespace

static_assert(vng::gfx::RecordType<Geometry>);
static_assert(vng::gfx::RecordType<Surface>);
static_assert(vng::gfx::Mesh<Geometry, Surface>::stream_count == 2);
static_assert(vng::gfx::Mesh<Geometry, Surface>::has(Position{}));
static_assert(vng::gfx::Mesh<Geometry, Surface>::has(Color{}));
static_assert(!vng::gfx::Mesh<Geometry, Surface>::has(Missing{}));
static_assert(std::is_trivially_copyable_v<vng::gfx::TriangleFace>);
static_assert(std::is_trivially_copyable_v<vng::gfx::Edge>);

TEST_CASE("a single-stream mesh exposes its CPU vertex stream", "[gfx][mesh]")
{
    vng::gfx::Mesh<Geometry> mesh(2);
    mesh.info().name = "two vertices";

    mesh.vertices()[0].set(Position{}, {1.0F, 2.0F, 3.0F});
    mesh.vertices()[1].set(Position{}, {4.0F, 5.0F, 6.0F});

    CHECK(mesh.vertex_count() == 2);
    CHECK_FALSE(mesh.empty());
    CHECK(mesh.info().name == "two vertices");

    const auto& const_mesh = mesh;
    const vng::Vec3 expected{4.0F, 5.0F, 6.0F};
    CHECK(const_mesh.vertices()[1].get(Position{}) == expected);
}

TEST_CASE("mesh helpers keep multiple vertex streams synchronized", "[gfx][mesh]")
{
    vng::gfx::Mesh<Geometry, Surface> mesh;
    mesh.reserve_vertices(4);

    Geometry geometry;
    geometry.set(Position{}, {1.0F, 2.0F, 3.0F});
    geometry.set(Normal{}, {0.0F, 0.0F, 1.0F});
    Surface surface;
    surface.set(Color{}, {1.0F, 0.5F, 0.0F, 1.0F});
    mesh.push_vertex(geometry, surface);

    CHECK(mesh.vertex_count() == 1);
    CHECK(mesh.vertices<Geometry>().size() == 1);
    CHECK(mesh.vertices<Surface>().size() == 1);
    CHECK(mesh.validate().has_value());

    mesh.resize_vertices(3);
    CHECK(mesh.vertices<Geometry>().size() == 3);
    CHECK(mesh.vertices<Surface>().size() == 3);

    vng::gfx::VertexStream<Geometry> geometry_stream(2);
    vng::gfx::VertexStream<Surface> surface_stream(2);
    vng::gfx::Mesh deduced{
        std::move(geometry_stream),
        std::move(surface_stream),
    };
    static_assert(std::same_as<
                  decltype(deduced),
                  vng::gfx::Mesh<Geometry, Surface>>);
    CHECK(deduced.validate().has_value());
}

TEST_CASE("mesh validation identifies a mismatched vertex stream", "[gfx][mesh]")
{
    vng::gfx::Mesh<Geometry, Surface> mesh(3);
    mesh.vertices<Surface>().resize(2);

    const auto result = mesh.validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code
          == vng::gfx::MeshDiagnosticCode::mismatched_stream_counts);
    REQUIRE(result.error().stream_index.has_value());
    CHECK(*result.error().stream_index == 1);
    REQUIRE(result.error().actual_stream_count.has_value());
    CHECK(*result.error().actual_stream_count == 2);
    CHECK(result.error().vertex_count == 3);

    CHECK_THROWS_AS(
        mesh.push_vertex(Geometry{}, Surface{}),
        std::logic_error);

    mesh.resize_vertices(4);
    CHECK(mesh.validate().has_value());
}

TEST_CASE("mesh push stages records before a stream can reallocate", "[gfx][mesh]")
{
    vng::gfx::Mesh<Geometry> mesh;
    Geometry original;
    original.set(Position{}, {3.0F, 2.0F, 1.0F});
    original.set(Normal{}, {0.0F, 1.0F, 0.0F});
    mesh.push_vertex(original);

    mesh.push_vertex(mesh.vertices()[0]);

    REQUIRE(mesh.vertex_count() == 2);
    CHECK(mesh.vertices()[1].get(Position{}) == vng::Vec3{3.0F, 2.0F, 1.0F});
    CHECK(mesh.vertices()[1].get(Normal{}) == vng::Vec3{0.0F, 1.0F, 0.0F});
}

TEST_CASE("faces are mutable and structural validation permits topology quality issues",
          "[gfx][mesh]")
{
    vng::gfx::Mesh<Geometry> mesh(3);
    mesh.add_face(0, 1, 2);
    mesh.faces().push_back({0, 0, 0});
    mesh.faces().push_back({0, 1, 2});
    mesh.add_edge(0, 0);
    mesh.add_edge(0, 1);
    mesh.add_edge(0, 1);

    REQUIRE(mesh.validate().has_value());
    CHECK(mesh.face_count() == 3);

    const auto& const_mesh = mesh;
    const vng::gfx::TriangleFace expected{0, 1, 2};
    CHECK(const_mesh.faces().front() == expected);
}

TEST_CASE("mesh topology fingerprints track the ordered indexed triangle list",
          "[gfx][mesh][topology]")
{
    vng::gfx::Mesh<Geometry> original(4);
    original.add_face(0, 1, 2);
    original.add_face(0, 2, 3);

    vng::gfx::Mesh<Surface> same_topology(4);
    same_topology.add_face(0, 1, 2);
    same_topology.add_face(0, 2, 3);
    CHECK(original.topology_fingerprint()
          == same_topology.topology_fingerprint());

    SECTION("face order is significant")
    {
        vng::gfx::Mesh<Geometry> reordered(4);
        reordered.add_face(0, 2, 3);
        reordered.add_face(0, 1, 2);
        CHECK(reordered.face_count() == original.face_count());
        CHECK(reordered.topology_fingerprint()
              != original.topology_fingerprint());
    }

    SECTION("winding and vertex references are significant")
    {
        vng::gfx::Mesh<Geometry> altered(4);
        altered.add_face(0, 2, 1);
        altered.add_face(0, 2, 3);
        CHECK(altered.face_count() == original.face_count());
        CHECK(altered.topology_fingerprint()
              != original.topology_fingerprint());
    }

    SECTION("the index domain is significant")
    {
        vng::gfx::Mesh<Geometry> extra_unreferenced_vertex(5);
        extra_unreferenced_vertex.add_face(0, 1, 2);
        extra_unreferenced_vertex.add_face(0, 2, 3);
        CHECK(extra_unreferenced_vertex.face_count() == original.face_count());
        CHECK(extra_unreferenced_vertex.topology_fingerprint()
              != original.topology_fingerprint());
    }

    SECTION("vertex data and non-rendered edge topology are excluded")
    {
        same_topology.vertices()[0].set(
            Color{}, {0.25F, 0.5F, 0.75F, 1.0F});
        same_topology.add_edge(3, 1);
        CHECK(same_topology.topology_fingerprint()
              == original.topology_fingerprint());
    }
}

TEST_CASE("explicit edges distinguish absent, present-empty, and populated",
          "[gfx][mesh]")
{
    vng::gfx::Mesh<Geometry> mesh(3);
    CHECK_FALSE(mesh.has_explicit_edges());
    CHECK(mesh.explicit_edge_count() == 0);
    CHECK_FALSE(mesh.explicit_edges().has_value());

    mesh.set_explicit_edges();
    REQUIRE(mesh.has_explicit_edges());
    REQUIRE(mesh.explicit_edges().has_value());
    CHECK(mesh.explicit_edges()->empty());
    CHECK(mesh.explicit_edge_count() == 0);

    mesh.add_edge(0, 2);
    REQUIRE(mesh.explicit_edges().has_value());
    REQUIRE(mesh.explicit_edges()->size() == 1);
    CHECK(mesh.explicit_edge_count() == 1);
    const vng::gfx::Edge expected{0, 2};
    CHECK(mesh.explicit_edges()->front() == expected);

    mesh.set_explicit_edges(std::vector<vng::gfx::Edge>{{1, 2}});
    REQUIRE(mesh.explicit_edges().has_value());
    const vng::gfx::Edge replacement{1, 2};
    CHECK(mesh.explicit_edges()->front() == replacement);

    mesh.omit_explicit_edges();
    CHECK_FALSE(mesh.has_explicit_edges());
    CHECK_FALSE(mesh.explicit_edges().has_value());
    CHECK(mesh.explicit_edge_count() == 0);
}

TEST_CASE("mesh validation pinpoints an out-of-range face index", "[gfx][mesh]")
{
    vng::gfx::Mesh<Geometry> mesh(3);
    mesh.add_face(0, 8, 2);

    const auto result = mesh.validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code
          == vng::gfx::MeshDiagnosticCode::face_index_out_of_bounds);
    REQUIRE(result.error().primitive_index.has_value());
    REQUIRE(result.error().primitive_vertex.has_value());
    REQUIRE(result.error().referenced_vertex.has_value());
    CHECK(*result.error().primitive_index == 0);
    CHECK(*result.error().primitive_vertex == 1);
    CHECK(*result.error().referenced_vertex == 8);
    CHECK(result.error().vertex_count == 3);
}

TEST_CASE("mesh validation pinpoints an out-of-range edge index", "[gfx][mesh]")
{
    vng::gfx::Mesh<Geometry> mesh(3);
    mesh.set_explicit_edges();
    mesh.add_edge(1, 7);

    const auto result = mesh.validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code
          == vng::gfx::MeshDiagnosticCode::edge_index_out_of_bounds);
    REQUIRE(result.error().primitive_index.has_value());
    REQUIRE(result.error().primitive_vertex.has_value());
    REQUIRE(result.error().referenced_vertex.has_value());
    CHECK(*result.error().primitive_index == 0);
    CHECK(*result.error().primitive_vertex == 1);
    CHECK(*result.error().referenced_vertex == 7);
    CHECK(result.error().vertex_count == 3);
}

TEST_CASE("declared vertex counts can be checked before allocating streams",
          "[gfx][mesh]")
{
    using Mesh = vng::gfx::Mesh<Geometry>;
    Mesh mesh;
    CHECK(Mesh::validate_vertex_count(3).has_value());

    if constexpr (std::numeric_limits<std::size_t>::max()
                  > std::numeric_limits<vng::u32>::max()) {
        const auto excessive = static_cast<std::size_t>(
            std::numeric_limits<vng::u32>::max()) + 1;
        CHECK_THROWS_AS(Mesh(excessive), std::length_error);
        CHECK_THROWS_AS(mesh.reserve_vertices(excessive), std::length_error);
        CHECK_THROWS_AS(mesh.resize_vertices(excessive), std::length_error);

        const auto result = Mesh::validate_vertex_count(
            std::numeric_limits<std::size_t>::max());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code
              == vng::gfx::MeshDiagnosticCode::too_many_vertices);
        CHECK(result.error().vertex_count
              == std::numeric_limits<std::size_t>::max());
    }
}
