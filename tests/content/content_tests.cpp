#include <vng/content/content.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace vm = vng::content::vmesh;

struct Position : vng::gfx::Semantic<vng::Vec3> {};
struct Normal : vng::gfx::Semantic<vng::Vec3> {};
struct Color : vng::gfx::Semantic<vng::Vec4> {};
struct ObjectId : vng::gfx::Semantic<vng::u32> {};

using PositionRecord = vng::gfx::Record<Position>;
using PackedVertex = vng::gfx::Record<
    vng::gfx::as<Color, vng::gfx::unorm8x4>,
    Position>;
using Geometry = vng::gfx::Record<Normal, Position>;
using Surface = vng::gfx::Record<
    ObjectId,
    vng::gfx::as<Color, vng::gfx::unorm8x4>>;

[[nodiscard]] std::string simple_source(
    std::string_view vertex_count = "3",
    std::string_view fields = "value : f32;",
    std::string_view rows = "[0];\n[1];\n[2];",
    std::string_view face_count = "1",
    std::string_view faces = "[0, 1, 2];",
    std::string_view edges = {})
{
    std::string source = "vmesh 1.0\ninfo {}\nvertices ";
    source += vertex_count;
    source += " {\nfields { ";
    source += fields;
    source += " }\ndata {\n";
    source += rows;
    source += "\n}\n}\nfaces ";
    source += face_count;
    source += " {\n";
    source += faces;
    source += "\n}\n";
    source += edges;
    return source;
}

void require_parse_error(
    std::string_view source,
    vng::content::ErrorCode expected,
    const vm::ReadOptions& options = {})
{
    const auto result = vm::parse_vmesh(source, options);
    INFO(source);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == expected);
    REQUIRE(result.error().location.has_value());
    CHECK(result.error().location->line >= 1);
    CHECK(result.error().location->column >= 1);
}

void require_write_error(
    const vm::Document& document,
    vng::content::ErrorCode expected,
    const vm::WriteOptions& options = {})
{
    const auto result = vm::write_vmesh(document, options);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == expected);
}

[[nodiscard]] vm::VertexField float_field(
    std::string name,
    vng::u8 components,
    std::vector<vng::f32> values)
{
    return vm::VertexField{
        .name = std::move(name),
        .type = vm::FieldType{
            .scalar = vm::ScalarType::Float32,
            .components = components,
        },
        .values = vm::FieldValues{std::move(values)},
    };
}

[[nodiscard]] vm::VertexField uint_field(
    std::string name,
    vng::u8 components,
    std::vector<vng::u32> values)
{
    return vm::VertexField{
        .name = std::move(name),
        .type = vm::FieldType{
            .scalar = vm::ScalarType::UInt32,
            .components = components,
        },
        .values = vm::FieldValues{std::move(values)},
    };
}

[[nodiscard]] vm::Document basic_document()
{
    return vm::Document{
        .metadata = {},
        .vertex_count = 3,
        .vertex_fields = {
            float_field("value", 1, {0.0F, 1.0F, 2.0F}),
        },
        .faces = {vng::gfx::TriangleFace{0, 1, 2}},
        .edges = std::nullopt,
    };
}

[[nodiscard]] vm::Document typed_document()
{
    return vm::Document{
        .metadata = {
            {"author/tool", "test"},
            {"name", "Typed triangle"},
        },
        .vertex_count = 3,
        .vertex_fields = {
            float_field("unused", 1, {10.0F, 20.0F, 30.0F}),
            float_field("surface/color", 4, {
                -1.0F, 0.0F, 0.5F, 2.0F,
                1.0F, 0.25F, 0.75F, 1.0F,
                0.0F, 1.0F, 0.0F, 1.0F,
            }),
            float_field("geometry/position", 3, {
                -1.0F, -1.0F, 0.0F,
                1.0F, -1.0F, 0.0F,
                0.0F, 1.0F, 0.0F,
            }),
        },
        .faces = {vng::gfx::TriangleFace{0, 1, 2}},
        .edges = std::vector<vng::gfx::Edge>{
            {0, 1},
            {1, 2},
            {2, 0},
        },
    };
}

[[nodiscard]] bool close(float left, float right, float tolerance = 1.0e-6F)
{
    return std::abs(left - right) <= tolerance;
}

} // namespace

TEST_CASE("vmesh parses metadata, scalar types, vectors, faces, and edges",
          "[content][vmesh][parse]")
{
    constexpr std::string_view source = R"(vmesh 1.0
info {
    name = "Complete triangle";
    source/tool = "hand";
}
vertices 3 {
    fields {
        position : f32x3;
        joints : i32x2;
        object/id : u32;
    }
    data {
        [-1, -1, 0] [-2, 3] [7];
        [1, -1, 0] [4, 5] [8];
        [0, 1, 0] [6, 7] [9];
    }
}
faces 1 {
    [0, 1, 2];
}
edges 3 {
    [0, 1];
    [1, 2];
    [2, 0];
}
)";

    const auto parsed = vm::parse_vmesh(source);
    REQUIRE(parsed.has_value());
    CHECK(parsed->metadata.at("name") == "Complete triangle");
    CHECK(parsed->metadata.at("source/tool") == "hand");
    CHECK(parsed->vertex_count == 3);
    REQUIRE(parsed->vertex_fields.size() == 3);

    CHECK(parsed->vertex_fields[0].type
          == vm::FieldType{vm::ScalarType::Float32, 3});
    CHECK(parsed->vertex_fields[1].type
          == vm::FieldType{vm::ScalarType::Int32, 2});
    CHECK(parsed->vertex_fields[2].type
          == vm::FieldType{vm::ScalarType::UInt32, 1});
    CHECK(std::get<std::vector<vng::f32>>(parsed->vertex_fields[0].values)
          == std::vector<vng::f32>{-1.0F, -1.0F, 0.0F,
                                   1.0F, -1.0F, 0.0F,
                                   0.0F, 1.0F, 0.0F});
    CHECK(std::get<std::vector<vng::i32>>(parsed->vertex_fields[1].values)
          == std::vector<vng::i32>{-2, 3, 4, 5, 6, 7});
    CHECK(std::get<std::vector<vng::u32>>(parsed->vertex_fields[2].values)
          == std::vector<vng::u32>{7, 8, 9});

    const vng::gfx::TriangleFace expected_face{0, 1, 2};
    CHECK(parsed->faces == std::vector<vng::gfx::TriangleFace>{expected_face});
    REQUIRE(parsed->edges.has_value());
    CHECK(parsed->edges->size() == 3);
}

TEST_CASE("vmesh accepts comments, CRLF, stable slash names, and string escapes",
          "[content][vmesh][parse]")
{
    constexpr std::string_view source =
        "# leading comment\r\n"
        "vmesh 1.0 # version\r\n"
        "info {\r\n"
        " name = \"quote: \\\" slash: \\\\ line:\\n omega:\\u03A9\"; # info\r\n"
        " build/2d = \"ok\";\r\n"
        "}\r\n"
        "vertices 3 {\r\n"
        " fields { value : f32; }\r\n"
        " data { [0]; [1]; [2]; }\r\n"
        "}\r\n"
        "faces 1 { [0, 1, 2]; } # final comment";

    const auto parsed = vm::parse_vmesh(source);
    REQUIRE(parsed.has_value());
    CHECK(parsed->metadata.at("name")
          == "quote: \" slash: \\ line:\n omega:\xCE\xA9");
    CHECK(parsed->metadata.at("build/2d") == "ok");
}

TEST_CASE("vmesh reports malformed headers, sections, names, and strings",
          "[content][vmesh][parse][error]")
{
    SECTION("unsupported version")
    {
        require_parse_error("vmesh 2.0", vng::content::ErrorCode::unsupported_version);
    }
    SECTION("missing required info section")
    {
        require_parse_error(
            "vmesh 1.0 vertices 0 {}",
            vng::content::ErrorCode::unexpected_token);
    }
    SECTION("invalid character")
    {
        require_parse_error("@", vng::content::ErrorCode::invalid_token);
    }
    SECTION("invalid stable name")
    {
        require_parse_error(
            "vmesh 1.0 info { bad//key = \"x\"; }",
            vng::content::ErrorCode::invalid_token);
    }
    SECTION("unknown escape")
    {
        require_parse_error(
            R"(vmesh 1.0 info { name = "\q"; })",
            vng::content::ErrorCode::invalid_escape);
    }
    SECTION("surrogate escape")
    {
        require_parse_error(
            R"(vmesh 1.0 info { name = "\uD800"; })",
            vng::content::ErrorCode::invalid_escape);
    }
    SECTION("invalid UTF-8")
    {
        std::string source = "vmesh 1.0 info { name = \"";
        source.push_back(static_cast<char>(0xFF));
        source += "\"; }";
        require_parse_error(source, vng::content::ErrorCode::invalid_utf8);
    }
}

TEST_CASE("vmesh rejects malformed declarations and row counts",
          "[content][vmesh][parse][error]")
{
    SECTION("negative vertex count")
    {
        require_parse_error(
            simple_source("-1", "value : f32;", {}, "0", {}),
            vng::content::ErrorCode::invalid_number);
    }
    SECTION("count outside uint64")
    {
        require_parse_error(
            simple_source("184467440737095516160", "value : f32;", {}, "0", {}),
            vng::content::ErrorCode::number_out_of_range);
    }
    SECTION("unsupported field type")
    {
        require_parse_error(
            simple_source("1", "value : f64;", "[0];", "0", {}),
            vng::content::ErrorCode::invalid_field_type);
    }
    SECTION("no fields")
    {
        require_parse_error(
            simple_source("0", {}, {}, "0", {}),
            vng::content::ErrorCode::count_mismatch);
    }
    SECTION("too few rows")
    {
        require_parse_error(
            simple_source("2", "value : f32;", "[0];", "0", {}),
            vng::content::ErrorCode::count_mismatch);
    }
    SECTION("too many rows")
    {
        require_parse_error(
            simple_source("1", "value : f32;", "[0]; [1];", "0", {}),
            vng::content::ErrorCode::count_mismatch);
    }
    SECTION("too few faces")
    {
        require_parse_error(
            simple_source("3", "value : f32;", "[0]; [1]; [2];", "2",
                          "[0, 1, 2];"),
            vng::content::ErrorCode::count_mismatch);
    }
    SECTION("too many faces")
    {
        require_parse_error(
            simple_source("3", "value : f32;", "[0]; [1]; [2];", "0",
                          "[0, 1, 2];"),
            vng::content::ErrorCode::count_mismatch);
    }
    SECTION("too few edges")
    {
        require_parse_error(
            simple_source("3", "value : f32;", "[0]; [1]; [2];", "0", {},
                          "edges 2 { [0, 1]; }"),
            vng::content::ErrorCode::count_mismatch);
    }
}

TEST_CASE("vmesh rejects malformed and out-of-range numeric values",
          "[content][vmesh][parse][error]")
{
    SECTION("malformed float")
    {
        require_parse_error(
            simple_source("1", "value : f32;", "[1.2.3];", "0", {}),
            vng::content::ErrorCode::invalid_number);
    }
    SECTION("float overflow")
    {
        require_parse_error(
            simple_source("1", "value : f32;", "[1e999];", "0", {}),
            vng::content::ErrorCode::number_out_of_range);
    }
    SECTION("signed overflow")
    {
        require_parse_error(
            simple_source("1", "value : i32;", "[-2147483649];", "0", {}),
            vng::content::ErrorCode::number_out_of_range);
    }
    SECTION("negative unsigned value")
    {
        require_parse_error(
            simple_source("1", "value : u32;", "[-1];", "0", {}),
            vng::content::ErrorCode::invalid_number);
    }
    SECTION("wrong vector arity")
    {
        require_parse_error(
            simple_source("1", "value : f32x3;", "[1, 2];", "0", {}),
            vng::content::ErrorCode::unexpected_token);
    }
}

TEST_CASE("vmesh validates indices but preserves topology quality issues",
          "[content][vmesh][parse]")
{
    SECTION("face index out of range")
    {
        require_parse_error(
            simple_source("3", "value : f32;", "[0]; [1]; [2];", "1",
                          "[0, 1, 3];"),
            vng::content::ErrorCode::index_out_of_range);
    }
    SECTION("negative face index")
    {
        require_parse_error(
            simple_source("3", "value : f32;", "[0]; [1]; [2];", "1",
                          "[0, -1, 2];"),
            vng::content::ErrorCode::invalid_number);
    }
    SECTION("edge index out of range")
    {
        require_parse_error(
            simple_source("3", "value : f32;", "[0]; [1]; [2];", "0", {},
                          "edges 1 { [0, 3]; }"),
            vng::content::ErrorCode::index_out_of_range);
    }
    SECTION("repeated corners, self edges, and duplicates remain representable")
    {
        const auto parsed = vm::parse_vmesh(simple_source(
            "3", "value : f32;", "[0]; [1]; [2];", "3",
            "[0, 1, 2]; [0, 0, 0]; [0, 1, 2];",
            "edges 3 { [0, 0]; [0, 1]; [1, 0]; }"));
        REQUIRE(parsed.has_value());
        CHECK(parsed->faces.size() == 3);
        REQUIRE(parsed->edges.has_value());
        CHECK(parsed->edges->size() == 3);

        const auto written = vm::write_vmesh(*parsed);
        REQUIRE(written.has_value());
        const auto round_trip = vm::parse_vmesh(*written);
        REQUIRE(round_trip.has_value());
        CHECK(*round_trip == *parsed);
    }
}

TEST_CASE("vmesh detects duplicate metadata and vertex fields",
          "[content][vmesh][parse][error]")
{
    require_parse_error(
        "vmesh 1.0 info { name = \"a\"; name = \"b\"; }",
        vng::content::ErrorCode::duplicate_metadata);

    require_parse_error(
        simple_source("1", "value : f32; value : f32;", "[0] [1];", "0", {}),
        vng::content::ErrorCode::duplicate_field);
}

TEST_CASE("vmesh enforces configured parser limits", "[content][vmesh][limits]")
{
    const auto source = simple_source();

    SECTION("source bytes")
    {
        vm::ReadOptions options;
        options.limits.max_source_bytes = source.size() - 1;
        require_parse_error(
            source, vng::content::ErrorCode::input_too_large, options);
    }
    SECTION("metadata entries")
    {
        vm::ReadOptions options;
        options.limits.max_metadata_entries = 0;
        require_parse_error(
            "vmesh 1.0 info { name = \"x\"; }",
            vng::content::ErrorCode::limit_exceeded,
            options);
    }
    SECTION("metadata value bytes are enforced while lexing")
    {
        vm::ReadOptions options;
        options.limits.max_metadata_value_bytes = 2;
        require_parse_error(
            "vmesh 1.0 info { name = \"abc\"; }",
            vng::content::ErrorCode::limit_exceeded,
            options);
    }
    SECTION("metadata key bytes")
    {
        vm::ReadOptions options;
        options.limits.max_metadata_key_bytes = 3;
        require_parse_error(
            "vmesh 1.0 info { name = \"x\"; }",
            vng::content::ErrorCode::limit_exceeded,
            options);
    }
    SECTION("vertex count")
    {
        vm::ReadOptions options;
        options.limits.max_vertices = 2;
        require_parse_error(source, vng::content::ErrorCode::limit_exceeded, options);
    }
    SECTION("hard u32 vertex-count limit")
    {
        if constexpr (std::numeric_limits<std::size_t>::max()
                      > std::numeric_limits<vng::u32>::max()) {
            vm::ReadOptions options;
            options.limits.max_vertices = std::numeric_limits<std::size_t>::max();
            require_parse_error(
                "vmesh 1.0 info {} vertices 4294967296",
                vng::content::ErrorCode::limit_exceeded,
                options);
        }
    }
    SECTION("field count")
    {
        vm::ReadOptions options;
        options.limits.max_vertex_fields = 0;
        require_parse_error(source, vng::content::ErrorCode::limit_exceeded, options);
    }
    SECTION("scalar count")
    {
        vm::ReadOptions options;
        options.limits.max_scalar_values = 2;
        require_parse_error(source, vng::content::ErrorCode::limit_exceeded, options);
    }
    SECTION("decoded byte budget")
    {
        vm::ReadOptions options;
        options.limits.max_decoded_bytes = 23;
        require_parse_error(source, vng::content::ErrorCode::limit_exceeded, options);
    }
    SECTION("face count")
    {
        vm::ReadOptions options;
        options.limits.max_faces = 0;
        require_parse_error(source, vng::content::ErrorCode::limit_exceeded, options);
    }
    SECTION("edge count")
    {
        vm::ReadOptions options;
        options.limits.max_edges = 0;
        require_parse_error(
            simple_source("3", "value : f32;", "[0]; [1]; [2];", "0", {},
                          "edges 1 { [0, 1]; }"),
            vng::content::ErrorCode::limit_exceeded,
            options);
    }
}

TEST_CASE("vmesh writer rejects structurally invalid Documents",
          "[content][vmesh][write][error]")
{
    SECTION("no vertex fields")
    {
        auto document = basic_document();
        document.vertex_fields.clear();
        require_write_error(document, vng::content::ErrorCode::invalid_document);
    }
    SECTION("wrong scalar vector length")
    {
        auto document = basic_document();
        std::get<std::vector<vng::f32>>(document.vertex_fields[0].values).pop_back();
        require_write_error(document, vng::content::ErrorCode::count_mismatch);
    }
    SECTION("declared scalar and variant disagree")
    {
        auto document = basic_document();
        document.vertex_fields[0].values = std::vector<vng::u32>{0, 1, 2};
        require_write_error(document, vng::content::ErrorCode::invalid_field_type);
    }
    SECTION("invalid component count")
    {
        auto document = basic_document();
        document.vertex_fields[0].type.components = 0;
        require_write_error(document, vng::content::ErrorCode::invalid_field_type);
    }
    SECTION("duplicate field")
    {
        auto document = basic_document();
        document.vertex_fields.push_back(document.vertex_fields.front());
        require_write_error(document, vng::content::ErrorCode::duplicate_field);
    }
    SECTION("non-finite float")
    {
        auto document = basic_document();
        std::get<std::vector<vng::f32>>(document.vertex_fields[0].values)[0]
            = std::numeric_limits<vng::f32>::infinity();
        require_write_error(document, vng::content::ErrorCode::invalid_number);
    }
    SECTION("face index out of range")
    {
        auto document = basic_document();
        document.faces[0] = {0, 1, 3};
        require_write_error(document, vng::content::ErrorCode::index_out_of_range);
    }
    SECTION("serialized byte limit")
    {
        auto document = basic_document();
        vm::WriteOptions options;
        options.limits.max_source_bytes = 8;
        require_write_error(
            document, vng::content::ErrorCode::input_too_large, options);
    }
    SECTION("decoded byte budget")
    {
        auto document = basic_document();
        vm::WriteOptions options;
        options.limits.max_decoded_bytes = 23;
        require_write_error(
            document, vng::content::ErrorCode::limit_exceeded, options);
    }
}

TEST_CASE("vmesh writing is deterministic and exactly round-trippable",
          "[content][vmesh][write]")
{
    vm::Document document{
        .metadata = {
            {"z", "quote \" slash \\"},
            {"a", "line\n"},
        },
        .vertex_count = 3,
        .vertex_fields = {
            uint_field("id", 1, {5, 6, 7}),
        },
        .faces = {vng::gfx::TriangleFace{0, 1, 2}},
        .edges = std::vector<vng::gfx::Edge>{},
    };

    constexpr std::string_view expected = R"(vmesh 1.0
info {
    a = "line\n";
    z = "quote \" slash \\";
}
vertices 3 {
    fields {
        id : u32;
    }
    data {
        [5];
        [6];
        [7];
    }
}
faces 1 {
    [0, 1, 2];
}
edges 0 {
}
)";

    const auto first = vm::write_vmesh(document);
    const auto second = vm::write_vmesh(document);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == expected);
    CHECK(*second == *first);

    const auto reparsed = vm::parse_vmesh(*first);
    REQUIRE(reparsed.has_value());
    CHECK(*reparsed == document);
}

TEST_CASE("vmesh text preserves every finite f32 bit pattern tested",
          "[content][vmesh][write]")
{
    const std::vector<vng::f32> values{
        0.0F,
        -0.0F,
        std::numeric_limits<vng::f32>::lowest(),
        std::numeric_limits<vng::f32>::max(),
        std::numeric_limits<vng::f32>::min(),
        std::numeric_limits<vng::f32>::denorm_min(),
        0.1F,
        -12345.678F,
    };
    vm::Document document{
        .metadata = {},
        .vertex_count = values.size(),
        .vertex_fields = {float_field("value", 1, values)},
        .faces = {},
        .edges = std::nullopt,
    };

    const auto text = vm::write_vmesh(document);
    REQUIRE(text.has_value());
    const auto reparsed = vm::parse_vmesh(*text);
    REQUIRE(reparsed.has_value());
    const auto& actual = std::get<std::vector<vng::f32>>(
        reparsed->vertex_fields[0].values);
    REQUIRE(actual.size() == values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        CHECK(std::bit_cast<vng::u32>(actual[index])
              == std::bit_cast<vng::u32>(values[index]));
    }
}

TEST_CASE("vmesh preserves absent and explicitly empty edge sections",
          "[content][vmesh][edges]")
{
    const auto absent = vm::parse_vmesh(simple_source());
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->edges.has_value());

    const auto present = vm::parse_vmesh(simple_source(
        "3", "value : f32;", "[0]; [1]; [2];", "1", "[0, 1, 2];",
        "edges 0 {}"));
    REQUIRE(present.has_value());
    REQUIRE(present->edges.has_value());
    CHECK(present->edges->empty());

    const auto absent_text = vm::write_vmesh(*absent);
    const auto present_text = vm::write_vmesh(*present);
    REQUIRE(absent_text.has_value());
    REQUIRE(present_text.has_value());
    CHECK(absent_text->find("edges") == std::string::npos);
    CHECK(present_text->find("edges 0") != std::string::npos);
}

TEST_CASE("zero-vertex vmesh documents survive the complete typed round trip",
          "[content][vmesh][schema]")
{
    constexpr std::string_view source = R"(vmesh 1.0
info {
    name = "empty";
}
vertices 0 {
    fields {
        position : f32x3;
    }
    data {
    }
}
faces 0 {
}
edges 0 {
}
)";

    const auto parsed = vm::parse_vmesh(source);
    REQUIRE(parsed.has_value());
    REQUIRE(vm::validate(*parsed).has_value());

    const auto mapping = vm::schema<PositionRecord>(
        vm::map("position", Position{}));
    const auto decoded = vm::decode(*parsed, mapping);
    REQUIRE(decoded.has_value());
    CHECK(decoded->empty());
    CHECK(decoded->faces().empty());
    REQUIRE(decoded->explicit_edges().has_value());
    CHECK(decoded->explicit_edges()->empty());

    const auto encoded = vm::encode(*decoded, mapping);
    REQUIRE(encoded.has_value());
    const auto written = vm::write_vmesh(*encoded);
    REQUIRE(written.has_value());
    const auto reparsed = vm::parse_vmesh(*written);
    REQUIRE(reparsed.has_value());
    CHECK(*reparsed == *encoded);
}

TEST_CASE("vmesh filesystem adapters preserve data and diagnostic paths",
          "[content][vmesh][io]")
{
    auto document = basic_document();
    const auto identity = reinterpret_cast<std::uintptr_t>(&document);
    const auto path = std::filesystem::temp_directory_path()
        / ("vng-content-" + std::to_string(identity) + ".vmesh");
    struct RemoveAtExit final {
        std::filesystem::path path;
        ~RemoveAtExit()
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};

    const auto saved = vm::write_vmesh(path, document);
    REQUIRE(saved.has_value());
    const auto loaded = vm::read_vmesh(path);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == document);

    const auto mapping = vm::schema<PositionRecord>(
        vm::map("position", Position{}));
    vng::gfx::Mesh<PositionRecord> typed_mesh(3);
    typed_mesh.info().name = "typed io";
    typed_mesh.vertices()[0].set(Position{}, {-1.0F, 0.0F, 0.0F});
    typed_mesh.vertices()[1].set(Position{}, {1.0F, 0.0F, 0.0F});
    typed_mesh.vertices()[2].set(Position{}, {0.0F, 1.0F, 0.0F});
    typed_mesh.add_face(0, 1, 2);
    REQUIRE(vm::save(path, typed_mesh, mapping).has_value());
    const auto loaded_mesh = vm::load(path, mapping);
    REQUIRE(loaded_mesh.has_value());
    CHECK(loaded_mesh->info() == typed_mesh.info());
    CHECK(loaded_mesh->faces() == typed_mesh.faces());
    CHECK(loaded_mesh->vertices()[2].get(Position{})
          == typed_mesh.vertices()[2].get(Position{}));

    auto invalid = document;
    invalid.faces[0] = {0, 1, 9};
    const auto invalid_write = vm::write_vmesh(path, invalid);
    REQUIRE_FALSE(invalid_write.has_value());
    CHECK(invalid_write.error().path == path);

    std::error_code ignored;
    REQUIRE(std::filesystem::remove(path, ignored));
    const auto missing = vm::read_vmesh(path);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == vng::content::ErrorCode::io_error);
    CHECK(missing.error().path == path);
}

TEST_CASE("one-stream schema decoding is name-based and applies target codecs",
          "[content][vmesh][schema]")
{
    const auto mapping = vm::schema<PackedVertex>(
        vm::map("geometry/position", Position{}),
        vm::map("surface/color", Color{}));
    const auto document = typed_document();

    const auto decoded = vm::decode(document, mapping);
    REQUIRE(decoded.has_value());
    CHECK(decoded->info().name == "Typed triangle");
    CHECK(decoded->vertex_count() == 3);
    CHECK(decoded->face_count() == 1);
    CHECK(decoded->has_explicit_edges());
    CHECK(decoded->explicit_edge_count() == 3);

    const auto first_position = decoded->vertices()[0].get(Position{});
    CHECK(first_position == vng::Vec3{-1.0F, -1.0F, 0.0F});
    const auto first_color = decoded->vertices()[0].get(Color{});
    CHECK(first_color.x == 0.0F);
    CHECK(first_color.y == 0.0F);
    CHECK(close(first_color.z, 128.0F / 255.0F, 1.0F / 255.0F));
    CHECK(first_color.w == 1.0F);

    const auto encoded = vm::encode(*decoded, mapping);
    REQUIRE(encoded.has_value());
    CHECK(encoded->metadata.size() == 1);
    CHECK(encoded->metadata.at("name") == "Typed triangle");
    REQUIRE(encoded->vertex_fields.size() == 2);
    CHECK(encoded->vertex_fields[0].name == "geometry/position");
    CHECK(encoded->vertex_fields[1].name == "surface/color");
    CHECK(encoded->faces == document.faces);
    CHECK(encoded->edges == document.edges);

    const auto& colors = std::get<std::vector<vng::f32>>(
        encoded->vertex_fields[1].values);
    CHECK(colors[0] == 0.0F);
    CHECK(close(colors[2], 128.0F / 255.0F, 1.0F / 255.0F));
    CHECK(colors[3] == 1.0F);
}

TEST_CASE("incremental schemas provide a direct name-to-semantic mapping",
          "[content][vmesh][schema]")
{
    auto mapping = vm::schema<PackedVertex>();
    mapping.map("geometry/position", Position{});
    mapping.map("surface/color", Color{});

    const auto decoded = vm::decode(typed_document(), mapping);
    REQUIRE(decoded.has_value());
    CHECK(decoded->vertices()[0].get(Position{})
          == vng::Vec3{-1.0F, -1.0F, 0.0F});
    CHECK(close(
        decoded->vertices()[0].get(Color{}).z,
        128.0F / 255.0F,
        1.0F / 255.0F));

    // Mapping call order, rather than Record storage order, controls the
    // deterministic field order produced by encode().
    const auto copied_mapping = mapping;
    const auto encoded = vm::encode(*decoded, copied_mapping);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->vertex_fields.size() == 2);
    CHECK(encoded->vertex_fields[0].name == "geometry/position");
    CHECK(encoded->vertex_fields[1].name == "surface/color");
}

TEST_CASE("incremental schemas diagnose unfinished and repeated mappings",
          "[content][vmesh][schema][error]")
{
    SECTION("missing semantic")
    {
        auto mapping = vm::schema<PackedVertex>();
        mapping.map("geometry/position", Position{});

        const auto decoded = vm::decode(typed_document(), mapping);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code
              == vng::content::ErrorCode::invalid_document);
        CHECK(decoded.error().message.find("maps 1 of 2")
              != std::string::npos);
        REQUIRE(decoded.error().notes.size() == 1);
        CHECK(decoded.error().notes[0].find("Color")
              != std::string::npos);

        vng::gfx::Mesh<PackedVertex> mesh(0);
        const auto encoded = vm::encode(mesh, mapping);
        REQUIRE_FALSE(encoded.has_value());
        CHECK(encoded.error().code
              == vng::content::ErrorCode::invalid_document);
    }

    SECTION("semantic mapped twice")
    {
        auto mapping = vm::schema<PackedVertex>();
        mapping.map("geometry/position", Position{});
        mapping.map("other/position", Position{});
        mapping.map("surface/color", Color{});

        const auto decoded = vm::decode(typed_document(), mapping);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code
              == vng::content::ErrorCode::duplicate_field);
        CHECK(decoded.error().message.find("Position")
              != std::string::npos);
        REQUIRE(decoded.error().notes.size() == 2);
        CHECK(decoded.error().notes[0]
              == "first file field: geometry/position");
        CHECK(decoded.error().notes[1]
              == "duplicate file field: other/position");
    }

    SECTION("duplicate file name")
    {
        auto mapping = vm::schema<PackedVertex>();
        mapping.map("shared", Position{}).map("shared", Color{});

        const auto decoded = vm::decode(typed_document(), mapping);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code
              == vng::content::ErrorCode::duplicate_field);
    }

    SECTION("invalid file name")
    {
        auto mapping = vm::schema<PackedVertex>();
        mapping.map("bad name", Position{});
        mapping.map("surface/color", Color{});

        const auto decoded = vm::decode(typed_document(), mapping);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code
              == vng::content::ErrorCode::invalid_document);
    }
}

TEST_CASE("multi-stream schemas decode reordered fields and encode in schema order",
          "[content][vmesh][schema]")
{
    auto document = typed_document();
    document.vertex_fields.insert(
        document.vertex_fields.begin() + 1,
        uint_field("object/id", 1, {41, 42, 43}));
    document.vertex_fields.push_back(float_field("geometry/normal", 3, {
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
    }));

    const auto geometry = vm::schema<Geometry>(
        vm::map("geometry/position", Position{}),
        vm::map("geometry/normal", Normal{}));
    const auto surface = vm::schema<Surface>(
        vm::map("surface/color", Color{}),
        vm::map("object/id", ObjectId{}));

    const auto decoded = vm::decode(document, geometry, surface);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->validate().has_value());
    CHECK(decoded->vertices<Geometry>()[2].get(Position{})
          == vng::Vec3{0.0F, 1.0F, 0.0F});
    CHECK(decoded->vertices<Geometry>()[1].get(Normal{})
          == vng::Vec3{0.0F, 0.0F, 1.0F});
    CHECK(decoded->vertices<Surface>()[1].get(ObjectId{}) == 42);

    const auto encoded = vm::encode(*decoded, geometry, surface);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->vertex_fields.size() == 4);
    CHECK(encoded->vertex_fields[0].name == "geometry/position");
    CHECK(encoded->vertex_fields[1].name == "geometry/normal");
    CHECK(encoded->vertex_fields[2].name == "surface/color");
    CHECK(encoded->vertex_fields[3].name == "object/id");
}

TEST_CASE("schema bridge diagnoses invalid names and document field contracts",
          "[content][vmesh][schema][error]")
{
    const auto position_schema = vm::schema<PositionRecord>(
        vm::map("position", Position{}));

    SECTION("empty schema name")
    {
        const auto empty_name = vm::schema<PositionRecord>(
            vm::map("", Position{}));
        const auto result = vm::decode(basic_document(), empty_name);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vng::content::ErrorCode::invalid_document);
    }
    SECTION("invalid schema name")
    {
        const auto invalid_name = vm::schema<PositionRecord>(
            vm::map("bad name", Position{}));
        const auto decoded = vm::decode(basic_document(), invalid_name);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == vng::content::ErrorCode::invalid_document);

        vng::gfx::Mesh<PositionRecord> mesh(3);
        const auto encoded = vm::encode(mesh, invalid_name);
        REQUIRE_FALSE(encoded.has_value());
        CHECK(encoded.error().code == vng::content::ErrorCode::invalid_document);
    }
    SECTION("duplicate schema file names")
    {
        using NormalRecord = vng::gfx::Record<Normal>;
        const auto positions = vm::schema<PositionRecord>(
            vm::map("shared", Position{}));
        const auto normals = vm::schema<NormalRecord>(
            vm::map("shared", Normal{}));
        const auto result = vm::decode(basic_document(), positions, normals);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vng::content::ErrorCode::duplicate_field);
    }
    SECTION("missing mapped field")
    {
        const auto result = vm::decode(basic_document(), position_schema);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vng::content::ErrorCode::invalid_document);
    }
    SECTION("wrong declared type")
    {
        auto document = basic_document();
        document.vertex_fields = {
            float_field("position", 2, {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F}),
        };
        const auto result = vm::decode(document, position_schema);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vng::content::ErrorCode::invalid_field_type);
    }
    SECTION("declared type and variant disagree")
    {
        auto document = basic_document();
        document.vertex_fields = {
            vm::VertexField{
                .name = "position",
                .type = {vm::ScalarType::Float32, 3},
                .values = std::vector<vng::u32>(9, 0),
            },
        };
        const auto result = vm::decode(document, position_schema);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vng::content::ErrorCode::invalid_field_type);
    }
    SECTION("mapped value count mismatch")
    {
        auto document = basic_document();
        document.vertex_fields = {
            float_field("position", 3, std::vector<vng::f32>(8, 0.0F)),
        };
        const auto result = vm::decode(document, position_schema);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vng::content::ErrorCode::count_mismatch);
    }
    SECTION("non-finite mapped value")
    {
        auto document = basic_document();
        document.vertex_fields = {
            float_field("position", 3, std::vector<vng::f32>(9, 0.0F)),
        };
        std::get<std::vector<vng::f32>>(document.vertex_fields[0].values)[4]
            = std::numeric_limits<vng::f32>::quiet_NaN();
        const auto result = vm::decode(document, position_schema);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vng::content::ErrorCode::invalid_number);
    }
    SECTION("malformed unselected field")
    {
        auto document = basic_document();
        document.vertex_fields = {
            float_field("position", 3, std::vector<vng::f32>(9, 0.0F)),
            float_field("unused", 2, std::vector<vng::f32>(5, 0.0F)),
        };
        const auto result = vm::decode(document, position_schema);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vng::content::ErrorCode::count_mismatch);
    }
    SECTION("duplicate document fields")
    {
        auto document = basic_document();
        auto position = float_field(
            "position", 3, std::vector<vng::f32>(9, 0.0F));
        document.vertex_fields = {position, position};
        const auto result = vm::decode(document, position_schema);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vng::content::ErrorCode::duplicate_field);
    }
}

TEST_CASE("schema encode carries MeshInfo name and rejects invalid mesh indices",
          "[content][vmesh][schema]")
{
    const auto mapping = vm::schema<PositionRecord>(
        vm::map("position", Position{}));
    vng::gfx::Mesh<PositionRecord> mesh(3);
    mesh.info().name = "Mesh name";
    mesh.add_face(0, 1, 3);

    const auto invalid = vm::encode(mesh, mapping);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == vng::content::ErrorCode::index_out_of_range);

    mesh.faces()[0] = {0, 1, 2};
    const auto encoded = vm::encode(mesh, mapping);
    REQUIRE(encoded.has_value());
    CHECK(encoded->metadata.at("name") == "Mesh name");

    mesh.vertices()[0].set(Position{}, {
        std::numeric_limits<vng::f32>::quiet_NaN(), 0.0F, 0.0F});
    const auto non_finite = vm::encode(mesh, mapping);
    REQUIRE_FALSE(non_finite.has_value());
    CHECK(non_finite.error().code == vng::content::ErrorCode::invalid_number);

    mesh.vertices()[0].set(Position{}, {0.0F, 0.0F, 0.0F});
    mesh.info().name = std::string(1, static_cast<char>(0xFF));
    const auto invalid_utf8 = vm::encode(mesh, mapping);
    REQUIRE_FALSE(invalid_utf8.has_value());
    CHECK(invalid_utf8.error().code == vng::content::ErrorCode::invalid_utf8);
}
