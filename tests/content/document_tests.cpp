#include <vng/content/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace document_test_types {

struct Pulse final {
    float frequency{};
    float minimum{};

    friend bool operator==(const Pulse&, const Pulse&) = default;
};

[[nodiscard]] vng::content::Result<Pulse> decode(
    vng::content::NodeView node, vng::content::Type<Pulse>)
{
    return node.read([](vng::content::Reader& reader) {
        return Pulse{
            .frequency = reader.get<float>("frequency"),
            .minimum = reader.get_or<float>("minimum", 0.0F),
        };
    });
}

struct ReactorSettings final {
    std::string mesh;
    float emission{};
    Pulse pulse;
};

struct MoveOnly final {
    explicit MoveOnly(int value) : value(std::make_unique<int>(value)) {}
    std::unique_ptr<int> value;
};

[[nodiscard]] vng::content::Result<MoveOnly> decode(
    vng::content::NodeView node, vng::content::Type<MoveOnly>)
{
    auto value = node.as<int>();
    if (!value) return std::unexpected(std::move(value.error()));
    return MoveOnly{*value};
}

} // namespace document_test_types

namespace {

namespace content = vng::content;

[[nodiscard]] std::string document_source(std::string_view properties)
{
    return "vscene 1.0\n" + std::string(properties);
}

} // namespace

TEST_CASE("Generic documents expose arbitrary object attributes without a scene schema",
          "[content][document][access]")
{
    auto document = content::parse_document(R"(vscene 1.0
objects = [
    {
        id = "reactor";
        kind = "pulsing_mesh";
        mesh = "meshes/reactor.vmesh";
        emission = 6;
        pulse = { frequency = 1.5; minimum = 0.2; };
    },
    {
        id = "door";
        kind = "sliding_door";
        travel = [0, 2.5, 0];
        opening_time = 0.8;
    }
];
editor = { color = "cyan"; locked = false; };
)");
    REQUIRE(document);
    auto objects_node = document->root().child("objects");
    REQUIRE(objects_node);
    auto objects = objects_node->elements();
    REQUIRE(objects);
    REQUIRE(objects->size() == 2);

    auto reactor = objects->at(0);
    auto door = objects->at(1);
    REQUIRE(reactor);
    REQUIRE(door);
    CHECK(reactor->get<std::string>("kind") == "pulsing_mesh");
    CHECK(reactor->get<float>("emission") == 6.0F);
    CHECK(door->get<vng::Vec3>("travel") == vng::Vec3{0.0F, 2.5F, 0.0F});
    CHECK_FALSE(door->get<float>("emission"));

    std::vector<std::string> ids;
    for (auto object : *objects) {
        auto id = object.get<std::string>("id");
        REQUIRE(id);
        ids.push_back(*id);
    }
    CHECK(ids == std::vector<std::string>{"reactor", "door"});
    CHECK_FALSE(objects->at(2));
    CHECK_FALSE(objects->at(std::numeric_limits<std::size_t>::max()));

    auto members = document->root().members();
    REQUIRE(members);
    CHECK(members->size() == 2);
    std::vector<std::string> names;
    for (auto member : *members) {
        names.emplace_back(member.name);
        CHECK(member.value.members().has_value() == (member.name == "editor"));
    }
    CHECK(names == std::vector<std::string>{"objects", "editor"});
}

TEST_CASE("Document property access distinguishes absent malformed and null values",
          "[content][document][access]")
{
    auto document = content::parse_document(document_source(R"(
number = 6;
bad_number = "very bright";
empty = null;
flag = true;
pulse.frequency = 9;
pulse = { frequency = 2; };
"odd key/with spaces" = 17;
)") );
    REQUIRE(document);
    auto root = document->root();

    CHECK(root.get_or<float>("absent", 4.0F) == 4.0F);
    CHECK(root.get_or<float>("number", 4.0F) == 6.0F);
    CHECK_FALSE(root.get_or<float>("bad_number", 4.0F));
    CHECK_FALSE(root.get_or<float>("empty", 4.0F));
    CHECK_FALSE(root.get<float>("absent"));

    auto missing = root.optional<float>("absent");
    auto present = root.optional<float>("number");
    REQUIRE(missing);
    REQUIRE(present);
    CHECK_FALSE(missing->has_value());
    CHECK(*present == 6.0F);
    CHECK_FALSE(root.optional<float>("bad_number"));
    CHECK_FALSE(root.optional<float>("empty"));
    CHECK(root.get<std::nullptr_t>("empty") == nullptr);
    CHECK_FALSE(root.get<std::nullptr_t>("absent"));

    CHECK(root.get<int>("pulse.frequency") == 9);
    auto pulse = root.child("pulse");
    REQUIRE(pulse);
    CHECK(pulse->get<int>("frequency") == 2);
    CHECK(root.get<int>("odd key/with spaces") == 17);

    auto scalar = root.child("number");
    REQUIRE(scalar);
    CHECK(scalar->as<float>() == 6.0F);
    CHECK_FALSE(scalar->get_or<float>("missing", 1.0F));
    CHECK_FALSE(scalar->optional<float>("missing"));
    CHECK_FALSE(scalar->child("missing"));
    CHECK_FALSE(scalar->elements());
    CHECK_FALSE(scalar->members());
    CHECK_FALSE(root.elements());
    CHECK_FALSE(root.as<float>());

    auto members = root.members();
    REQUIRE(members);
    CHECK(members->size() == 7);
}

TEST_CASE("Document numbers convert explicitly with range and integrality checks",
          "[content][document][numeric]")
{
    auto document = content::parse_document(document_source(R"(
integer = 6;
integral_float = 6.0;
fractional = 6.5;
negative = -1;
negative_zero = -0.0;
i64_min = -9223372036854775808;
i64_max = 9223372036854775807;
u64_max = 18446744073709551615;
over_i32 = 2147483648;
over_u32 = 4294967296;
float_overflow = 1e100;
boolean = true;
string = "6";
)") );
    REQUIRE(document);
    auto root = document->root();

    CHECK(root.get<float>("integer") == 6.0F);
    CHECK(root.get<double>("integer") == 6.0);
    CHECK(root.get<int>("integral_float") == 6);
    CHECK(root.get<unsigned>("integral_float") == 6U);
    CHECK_FALSE(root.get<int>("fractional"));
    CHECK_FALSE(root.get<unsigned>("negative"));
    CHECK(root.get<unsigned>("negative_zero") == 0U);
    auto negative_zero = root.get<double>("negative_zero");
    REQUIRE(negative_zero);
    CHECK(std::signbit(*negative_zero));

    CHECK(root.get<std::int64_t>("i64_min") == std::numeric_limits<std::int64_t>::min());
    CHECK(root.get<std::int64_t>("i64_max") == std::numeric_limits<std::int64_t>::max());
    CHECK(root.get<std::uint64_t>("u64_max") == std::numeric_limits<std::uint64_t>::max());
    CHECK_FALSE(root.get<std::uint64_t>("i64_min"));
    CHECK_FALSE(root.get<std::int64_t>("u64_max"));
    CHECK_FALSE(root.get<std::int32_t>("over_i32"));
    CHECK_FALSE(root.get<std::uint32_t>("over_u32"));
    CHECK_FALSE(root.get<float>("float_overflow"));
    CHECK(root.get<double>("float_overflow") == 1e100);
    CHECK_FALSE(root.get<float>("boolean"));
    CHECK_FALSE(root.get<int>("string"));
    CHECK_FALSE(root.get<bool>("integer"));
    CHECK_FALSE(root.get<std::string>("integer"));
    CHECK(root.get<bool>("boolean") == true);
}

TEST_CASE("Document vectors and containers decode element types and exact shapes",
          "[content][document][numeric]")
{
    auto document = content::parse_document(document_source(R"(
position = [1, -2.5, 3];
indices = [0, 1, 4294967295];
names = ["first", "second"];
empty = [];
matrix_rows = [[1, 2], [3, 4]];
bad_component = [0, "one", 2];
fractional_component = [0, 1.5, 2];
)") );
    REQUIRE(document);
    auto root = document->root();
    CHECK(root.get<vng::Vec3>("position") == vng::Vec3{1.0F, -2.5F, 3.0F});
    CHECK(root.get<std::array<float, 3>>("position") == std::array<float, 3>{1.0F, -2.5F, 3.0F});
    CHECK(root.get<vng::UVec3>("indices") == vng::UVec3{0, 1, 4294967295U});
    CHECK_FALSE(root.get<vng::IVec3>("indices"));
    CHECK_FALSE(root.get<vng::Vec2>("position"));
    CHECK_FALSE(root.get<vng::Vec4>("position"));
    CHECK_FALSE(root.get<vng::Vec3>("bad_component"));
    CHECK_FALSE(root.get<vng::IVec3>("fractional_component"));
    CHECK(root.get<std::vector<std::string>>("names")
          == std::vector<std::string>{"first", "second"});
    CHECK(root.get<std::vector<float>>("empty") == std::vector<float>{});
    CHECK(root.get<std::array<float, 0>>("empty") == std::array<float, 0>{});
    CHECK(root.get<std::vector<std::array<int, 2>>>("matrix_rows")
          == std::vector<std::array<int, 2>>{{1, 2}, {3, 4}});
}

TEST_CASE("Scoped document readers return expected results and stop at the first bad read",
          "[content][document][reader]")
{
    static_assert(std::same_as<
        content::Result<document_test_types::ReactorSettings>,
        std::expected<document_test_types::ReactorSettings, content::Diagnostic>>);

    auto document = content::parse_document(document_source(R"(
mesh = "meshes/reactor.vmesh";
emission = 6;
pulse = { frequency = 1.5; minimum = 0.2; };
)") );
    REQUIRE(document);
    auto settings = document->root().read([](content::Reader& reader) {
        return document_test_types::ReactorSettings{
            .mesh = reader.get<std::string>("mesh"),
            .emission = reader.get_or<float>("emission", 0.0F),
            .pulse = {
                .frequency = reader.child("pulse").get<float>("frequency"),
                .minimum = reader.child("pulse").get_or<float>("minimum", 0.0F),
            },
        };
    });
    REQUIRE(settings);
    CHECK(settings->mesh == "meshes/reactor.vmesh");
    CHECK(settings->emission == 6.0F);
    CHECK(settings->pulse == document_test_types::Pulse{1.5F, 0.2F});

    bool reached_after_failure = false;
    auto failed = document->root().read([&](auto& reader) {
        const auto invalid = reader.template get<float>("mesh");
        reached_after_failure = true;
        return invalid;
    });
    REQUIRE_FALSE(failed);
    CHECK_FALSE(reached_after_failure);

    CHECK_THROWS_AS(document->root().read([](auto&) -> int {
        throw std::runtime_error("an application exception, not a decoding diagnostic");
    }), std::runtime_error);

    auto optional = document->root().read([](auto& reader) {
        return reader.template optional<float>("absent");
    });
    REQUIRE(optional);
    CHECK_FALSE(optional->has_value());
}

TEST_CASE("Document custom decoders use ADL without central registration",
          "[content][document][reader]")
{
    auto document = content::parse_document(document_source(R"(
pulse = { frequency = 1.5; };
bad_pulse = { frequency = "fast"; };
pulses = [{ frequency = 1; }, { frequency = 2; minimum = 0.5; }];
)") );
    REQUIRE(document);
    auto pulse = document->root().get<document_test_types::Pulse>("pulse");
    REQUIRE(pulse);
    CHECK(*pulse == document_test_types::Pulse{1.5F, 0.0F});
    CHECK_FALSE(document->root().get<document_test_types::Pulse>("bad_pulse"));
    CHECK(document->root().get<std::vector<document_test_types::Pulse>>("pulses")
          == std::vector<document_test_types::Pulse>{{1.0F, 0.0F}, {2.0F, 0.5F}});

    auto from_reader = document->root().read([](auto& reader) {
        return reader.template get<document_test_types::Pulse>("pulse");
    });
    REQUIRE(from_reader);
    CHECK(*from_reader == *pulse);
}

TEST_CASE("Document serialization preserves unknown fields values and deterministic order",
          "[content][document][roundtrip]")
{
    const auto source = document_source(R"(
unknown_plugin = {
    custom = [null, true, false, -12, 18446744073709551615, 1.25e-10];
    "name with spaces" = "snowman: \u2603 and music: \uD834\uDD1E";
    control = "line one\nline two\t\"quoted\"\\slash";
    nested = { empty_object = {}; empty_array = []; };
};
objects = [];
)");
    auto document = content::parse_document(source);
    REQUIRE(document);
    auto serialized = content::write_document(*document);
    REQUIRE(serialized);
    auto second = content::parse_document(*serialized);
    REQUIRE(second);
    auto again = content::write_document(*second);
    REQUIRE(again);
    CHECK(*again == *serialized);

    auto plugin = second->root().child("unknown_plugin");
    REQUIRE(plugin);
    CHECK(plugin->get<std::string>("name with spaces")
          == "snowman: \xE2\x98\x83 and music: \xF0\x9D\x84\x9E");
    CHECK(plugin->get<std::string>("control") == "line one\nline two\t\"quoted\"\\slash");
    auto custom = plugin->child("custom");
    REQUIRE(custom);
    auto values = custom->elements();
    REQUIRE(values);
    REQUIRE(values->size() == 6);
    CHECK(values->at(0)->as<std::nullptr_t>() == nullptr);
    CHECK(values->at(1)->as<bool>() == true);
    CHECK(values->at(2)->as<bool>() == false);
    CHECK(values->at(3)->as<int>() == -12);
    CHECK(values->at(4)->as<std::uint64_t>() == std::numeric_limits<std::uint64_t>::max());
    CHECK(values->at(5)->as<double>() == 1.25e-10);
}

TEST_CASE("Document parser rejects duplicate keys malformed syntax and unsupported values",
          "[content][document][parse]")
{
    for (std::string_view invalid : {
        "vscene 2.0\nvalue = 1;",
        "vscene 1.0\nvalue = 1; value = 2;",
        "vscene 1.0\nparent = { value = 1; value = 2; };",
        "vscene 1.0\nvalue = [1, 2;",
        "vscene 1.0\nvalue = { key = 1;",
        "vscene 1.0\nvalue = unknown;",
        "vscene 1.0\nvalue = 18446744073709551616;",
        "vscene 1.0\nvalue = -9223372036854775809;",
        "vscene 1.0\nvalue = 1e400;",
        "vscene 1.0\nvalue = 1e;",
        "vscene 1.0\nvalue = nan;",
        "vscene 1.0\nvalue = inf;",
        "vscene 1.0\nvalue = \"unterminated;",
        "vscene 1.0\nvalue = \"\\q\";",
        "vscene 1.0\nvalue = \"\\uD800\";",
        "vscene 1.0\nvalue = \"\\uDC00\";",
        "vscene 1.0\nvalue = \"raw\nnewline\";",
    }) {
        INFO(invalid);
        auto result = content::parse_document(invalid);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().location);
        CHECK(result.error().location->line >= 1);
        CHECK(result.error().location->column >= 1);
        CHECK_FALSE(result.error().message.empty());
    }

    for (const auto& bad_utf8 : {
        std::string("\x80"),
        std::string("\xC0\xAF"),
        std::string("\xED\xA0\x80"),
        std::string("\xF4\x90\x80\x80"),
        std::string("\xE2\x82"),
    }) {
        auto result = content::parse_document(document_source("value = \"" + bad_utf8 + "\";"));
        CHECK_FALSE(result);
    }
}

TEST_CASE("Document diagnostics retain source files locations and complete property paths",
          "[content][document][diagnostic]")
{
    content::DocumentReadOptions options;
    options.source_path = "scenes/room.vscene";
    auto document = content::parse_document(R"(vscene 1.0
objects = [
    {
        pulse = { frequency = "fast"; };
        vector = [0, "wrong", 2];
    }
];
)", options);
    REQUIRE(document);
    CHECK(document->source_path() == options.source_path);
    auto objects_node = document->root().child("objects");
    REQUIRE(objects_node);
    auto objects = objects_node->elements();
    REQUIRE(objects);
    auto object = objects->at(0);
    REQUIRE(object);

    auto decoded = object->get<document_test_types::Pulse>("pulse");
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error().path == options.source_path);
    CHECK(decoded.error().property_path == "objects[0].pulse.frequency");
    REQUIRE(decoded.error().location);
    CHECK(decoded.error().location->line == 4);
    CHECK(decoded.error().location->column == 31);

    auto bad_vector = object->get<vng::Vec3>("vector");
    REQUIRE_FALSE(bad_vector);
    CHECK(bad_vector.error().property_path == "objects[0].vector[1]");
    REQUIRE(bad_vector.error().location);
    CHECK(bad_vector.error().location->line == 5);

    auto pulse = object->child("pulse");
    REQUIRE(pulse);
    auto missing = pulse->get<float>("minimum");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().property_path == "objects[0].pulse.minimum");
    CHECK(missing.error().path == options.source_path);
    REQUIRE(missing.error().location);

    auto parse_error = content::parse_document("vscene 1.0\nparent = { value = 1; value = 2; };", options);
    REQUIRE_FALSE(parse_error);
    CHECK(parse_error.error().path == options.source_path);
    CHECK(parse_error.error().property_path == "parent.value");
}

TEST_CASE("Document readers can report application validation failures without swallowing exceptions",
          "[content][document][reader]")
{
    auto document = content::parse_document(document_source("emission = -3;"));
    REQUIRE(document);
    auto result = document->root().read([](auto& reader) {
        const auto emission = reader.template get<float>("emission");
        if (emission < 0.0F) {
            reader.child("emission").fail("Emission must be nonnegative");
        }
        return emission;
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "Emission must be nonnegative");
    CHECK(result.error().property_path == "emission");

    bool completed = false;
    auto no_value = document->root().read([&](auto&) {
        completed = true;
    });
    static_assert(std::same_as<decltype(no_value), content::Result<void>>);
    REQUIRE(no_value);
    CHECK(completed);
}

TEST_CASE("Document limits bound source decoded storage strings numbers nodes and nesting",
          "[content][document][limits]")
{
    const auto source = document_source("first = 1; second = 2;");
    content::DocumentReadOptions options;
    options.limits.max_source_bytes = source.size() - 1;
    auto source_limit = content::parse_document(source, options);
    REQUIRE_FALSE(source_limit);
    CHECK(source_limit.error().code == content::ErrorCode::input_too_large);
    options.limits.max_source_bytes = source.size();
    CHECK(content::parse_document(source, options).has_value());

    options = {};
    options.limits.max_nodes = 2;
    CHECK_FALSE(content::parse_document(source, options));
    options.limits.max_nodes = 3;
    CHECK(content::parse_document(source, options).has_value());

    options = {};
    options.limits.max_decoded_bytes = 16;
    CHECK_FALSE(content::parse_document(source, options));

    options = {};
    options.limits.max_string_bytes = 7;
    CHECK_FALSE(content::parse_document(document_source("value = \"0123456789\";"), options));

    options = {};
    options.limits.max_number_bytes = 4;
    CHECK_FALSE(content::parse_document(document_source("value = 12345;"), options));

    options = {};
    options.limits.max_depth = 2;
    CHECK_FALSE(content::parse_document(document_source("a = { b = { c = 1; }; };"), options));
    CHECK_FALSE(content::parse_document(document_source("a = [[[1]]];"), options));
    options.limits.max_depth = 4;
    CHECK(content::parse_document(document_source("a = [[[1]]];"), options).has_value());

    std::string deeply_nested = "vscene 1.0\na = ";
    deeply_nested.append(300, '[');
    deeply_nested += '0';
    deeply_nested.append(300, ']');
    deeply_nested += ';';
    options.limits.max_depth = std::numeric_limits<std::size_t>::max();
    CHECK_FALSE(content::parse_document(deeply_nested, options));

    auto document = content::parse_document(source);
    REQUIRE(document);
    content::DocumentWriteOptions write_options;
    write_options.limits.max_source_bytes = 4;
    CHECK_FALSE(content::write_document(*document, write_options));
}

TEST_CASE("Documents own parsed strings while node views survive owner moves and copies",
          "[content][document][lifetime]")
{
    std::string text = "settings 1.0\nname = \"authored value\"; number = 6;";
    auto parsed = content::parse_document(text);
    REQUIRE(parsed);
    CHECK(parsed->kind() == "settings");
    CHECK(parsed->version().major == 1);
    CHECK(parsed->version().minor == 0);
    auto root = parsed->root();
    auto name = root.get<std::string>("name");
    REQUIRE(name);
    text.assign(text.size(), '!');
    CHECK(root.get<std::string>("name") == "authored value");

    std::optional<content::Document> moved{std::move(*parsed)};
    CHECK_FALSE(parsed->root().get<int>("number"));
    CHECK(root.get<int>("number") == 6);
    CHECK(moved->root().get<int>("number") == 6);
    std::optional<content::Document> copy{*moved};
    moved.reset();
    CHECK(root.get<int>("number") == 6);
    CHECK(copy->root().get<int>("number") == 6);
    copy.reset();
    // Node views borrow the owner and are deliberately not used after this point.
    CHECK(*name == "authored value");

    content::NodeView empty;
    CHECK_FALSE(empty.as<int>());
    CHECK_FALSE(empty.child("anything"));
    CHECK_FALSE(empty.get_or<int>("anything", 42));
    CHECK_FALSE(empty.optional<int>("anything"));
    CHECK_FALSE(empty.members());
    CHECK_FALSE(empty.elements());
}

TEST_CASE("Generic document filesystem adapters preserve source paths and unknown properties",
          "[content][document][io]")
{
    content::DocumentReadOptions read_options;
    read_options.source_path = "authored/source.vscene";
    auto document = content::parse_document(
        document_source("custom = { future = [1, 2, 3]; };"), read_options);
    REQUIRE(document);
    const auto identity = reinterpret_cast<std::uintptr_t>(&document);
    const auto path = std::filesystem::temp_directory_path()
        / ("vng-generic-document-" + std::to_string(identity) + ".vscene");
    struct RemoveAtExit final {
        std::filesystem::path path;
        ~RemoveAtExit()
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};

    REQUIRE(content::write_document(path, *document));
    auto loaded = content::read_document(path);
    REQUIRE(loaded);
    CHECK(loaded->source_path() == path);
    auto loaded_text = content::write_document(*loaded);
    auto original_text = content::write_document(*document);
    REQUIRE(loaded_text);
    REQUIRE(original_text);
    CHECK(*loaded_text == *original_text);
    auto custom = loaded->root().child("custom");
    REQUIRE(custom);
    CHECK(custom->get<std::vector<int>>("future") == std::vector<int>{1, 2, 3});

    content::DocumentWriteOptions invalid_write_options;
    invalid_write_options.limits.max_depth = 1;
    auto invalid_write = content::write_document(path, *document, invalid_write_options);
    REQUIRE_FALSE(invalid_write);
    CHECK(invalid_write.error().path == read_options.source_path);
    CHECK(invalid_write.error().property_path == "custom.future");
    bool identifies_destination{};
    for (const auto& note : invalid_write.error().notes) {
        identifies_destination |= note.find(path.string()) != std::string::npos;
    }
    CHECK(identifies_destination);
    auto untouched = content::read_document(path);
    REQUIRE(untouched);
    auto untouched_text = content::write_document(*untouched);
    REQUIRE(untouched_text);
    CHECK(*untouched_text == *original_text);

    content::DocumentReadOptions limited;
    limited.limits.max_source_bytes = 1;
    auto too_large = content::read_document(path, limited);
    REQUIRE_FALSE(too_large);
    CHECK(too_large.error().path == path);

    std::error_code ignored;
    REQUIRE(std::filesystem::remove(path, ignored));
    auto absent = content::read_document(path);
    REQUIRE_FALSE(absent);
    CHECK(absent.error().code == content::ErrorCode::io_error);
    CHECK(absent.error().path == path);
}

TEST_CASE("Scoped document reader iteration retains direct typed access and diagnostics",
          "[content][document][reader]")
{
    static_assert(std::ranges::forward_range<content::ArrayView>);
    static_assert(std::ranges::forward_range<content::ObjectView>);
    auto document = content::parse_document(document_source(R"(
objects = [{ id = "first"; emission = 1; }, { id = "second"; emission = 2; }];
attributes = { alpha = 3; beta = 4; };
bad = [1, 2, "three", 4];
)") );
    REQUIRE(document);

    auto names = document->read([](content::Reader& reader) {
        std::vector<std::string> names;
        for (auto object : reader.child("objects").elements()) {
            names.push_back(object.get<std::string>("id"));
        }
        return names;
    });
    CHECK(names == std::vector<std::string>{"first", "second"});

    auto sum = document->read([](content::Reader& reader) {
        int total{};
        for (auto [name, value] : reader.child("attributes").members()) {
            CHECK_FALSE(name.empty());
            total += value.as<int>();
        }
        return total;
    });
    CHECK(sum == 7);

    std::size_t visits{};
    auto failed = document->read([&](content::Reader& reader) {
        int sum{};
        for (auto value : reader.child("bad").elements()) {
            ++visits;
            sum += value.as<int>();
        }
        return sum;
    });
    REQUIRE_FALSE(failed);
    CHECK(visits == 3);
    CHECK(failed.error().property_path == "bad[2]");
}

TEST_CASE("Document custom values need neither default construction nor copying",
          "[content][document][reader]")
{
    auto document = content::parse_document(document_source("values = [3, 7]; scalar = 11;"));
    REQUIRE(document);
    auto fixed = document->root().get<std::array<document_test_types::MoveOnly, 2>>("values");
    REQUIRE(fixed);
    CHECK(*(*fixed)[0].value == 3);
    CHECK(*(*fixed)[1].value == 7);
    auto dynamic = document->root().get<std::vector<document_test_types::MoveOnly>>("values");
    REQUIRE(dynamic);
    REQUIRE(dynamic->size() == 2);
    CHECK(*(*dynamic)[1].value == 7);
    auto scoped = document->read([](content::Reader& reader) {
        return reader.get<document_test_types::MoveOnly>("scalar");
    });
    REQUIRE(scoped);
    CHECK(*scoped->value == 11);
}

TEST_CASE("Document numeric boundaries never overflow casts or silently underflow floats",
          "[content][document][numeric]")
{
    auto document = content::parse_document(document_source(R"(
i64_end_float = 9223372036854775808.0;
i64_last_float = 9223372036854774784.0;
u64_end_float = 18446744073709551616.0;
u64_last_float = 18446744073709549568.0;
tiny = 1e-100;
subnormal = 1e-45;
negative_zero = -0.0;
integral_float = 6.0;
)") );
    REQUIRE(document);
    auto root = document->root();
    CHECK_FALSE(root.get<std::int64_t>("i64_end_float"));
    CHECK(root.get<std::int64_t>("i64_last_float") == INT64_C(9223372036854774784));
    CHECK_FALSE(root.get<std::uint64_t>("u64_end_float"));
    CHECK(root.get<std::uint64_t>("u64_last_float") == UINT64_C(18446744073709549568));
    CHECK_FALSE(root.get<float>("tiny"));
    auto subnormal = root.get<float>("subnormal");
    REQUIRE(subnormal);
    CHECK(*subnormal > 0.0F);
    CHECK(*subnormal < std::numeric_limits<float>::min());

    auto text = content::write_document(*document);
    REQUIRE(text);
    auto roundtrip = content::parse_document(*text);
    REQUIRE(roundtrip);
    auto negative_zero = roundtrip->root().get<double>("negative_zero");
    REQUIRE(negative_zero);
    CHECK(std::signbit(*negative_zero));
    auto integral_float = roundtrip->root().child("integral_float");
    REQUIRE(integral_float);
    CHECK(integral_float->kind() == content::ValueKind::number);
}

TEST_CASE("Document literal key diagnostics cannot be confused with nested paths",
          "[content][document][diagnostic]")
{
    auto document = content::parse_document(document_source(R"(
"a.b" = "bad";
a = { b = "bad"; };
"line\nname" = "bad";
"quoted\"name" = "bad";
"slash/name" = "bad";
)") );
    REQUIRE(document);
    auto literal = document->root().get<int>("a.b");
    REQUIRE_FALSE(literal);
    CHECK(literal.error().property_path == "[\"a.b\"]");
    auto a = document->root().child("a");
    REQUIRE(a);
    auto nested = a->get<int>("b");
    REQUIRE_FALSE(nested);
    CHECK(nested.error().property_path == "a.b");
    auto newline = document->root().get<int>("line\nname");
    REQUIRE_FALSE(newline);
    CHECK(newline.error().property_path == "[\"line\\u000aname\"]");
    auto quoted = document->root().get<int>("quoted\"name");
    REQUIRE_FALSE(quoted);
    CHECK(quoted.error().property_path == "[\"quoted\\\"name\"]");
    auto slash = document->root().get<int>("slash/name");
    REQUIRE_FALSE(slash);
    CHECK(slash.error().property_path == "[\"slash/name\"]");
}

TEST_CASE("Document parsing safely rejects or deterministically roundtrips mutated inputs",
          "[content][document][parse]")
{
    const std::string original = R"(vscene 1.0
objects = [{ id = "reactor"; pulse = { frequency = 1.5; }; color = [0, 1, 1]; }];
plugin = { enabled = true; optional = null; label = "\u2603"; };
)";
    std::mt19937 random(0x45AFC218U);
    for (unsigned iteration = 0; iteration < 512; ++iteration) {
        auto input = original;
        const auto mutations = 1U + random() % 8U;
        for (unsigned mutation = 0; mutation < mutations; ++mutation) {
            const auto position = static_cast<std::size_t>(random()) % input.size();
            input[position] = static_cast<char>(random() % 256U);
        }
        auto document = content::parse_document(input);
        if (!document) {
            CHECK_FALSE(document.error().message.empty());
            continue;
        }
        auto text = content::write_document(*document);
        REQUIRE(text);
        auto second = content::parse_document(*text);
        REQUIRE(second);
        auto again = content::write_document(*second);
        REQUIRE(again);
        CHECK(*again == *text);
    }
}

TEST_CASE("Document reader and writer apply number token limits to the version header",
          "[content][document][limits]")
{
    auto document = content::parse_document("vscene 1.0\n");
    REQUIRE(document);
    for (std::size_t limit = 0; limit < 3; ++limit) {
        content::DocumentReadOptions read_options;
        read_options.limits.max_number_bytes = limit;
        auto read = content::parse_document("vscene 1.0\n", read_options);
        REQUIRE_FALSE(read);
        CHECK(read.error().code == content::ErrorCode::limit_exceeded);

        content::DocumentWriteOptions write_options;
        write_options.limits.max_number_bytes = limit;
        auto written = content::write_document(*document, write_options);
        REQUIRE_FALSE(written);
        CHECK(written.error().code == content::ErrorCode::limit_exceeded);
    }
    content::DocumentWriteOptions write_options;
    write_options.limits.max_number_bytes = 3;
    auto written = content::write_document(*document, write_options);
    REQUIRE(written);
    content::DocumentReadOptions read_options;
    read_options.limits = write_options.limits;
    CHECK(content::parse_document(*written, read_options).has_value());
}
