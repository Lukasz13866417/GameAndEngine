#include "support/spaceflight_scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace scene = example::spaceflight;
namespace content = vng::content;

content::Result<scene::Scene> parse(std::string_view properties)
{
    auto document = content::parse_document("vscene 1.0\n" + std::string(properties),
        {.source_path = "/example/scenes/flyby.vscene"});
    if (!document) return std::unexpected(std::move(document.error()));
    return scene::decode_scene(*document);
}

content::Result<scene::Options> options(std::initializer_list<const char*> arguments)
{
    std::vector<std::string> strings{"spaceflight"};
    for (const auto* argument : arguments) strings.emplace_back(argument);
    std::vector<char*> argv;
    for (auto& string : strings) argv.push_back(string.data());
    return scene::parse_options(static_cast<int>(argv.size()), argv.data(), "default.vscene");
}

} // namespace

TEST_CASE("Spaceflight scene conventions remain outside the document engine",
    "[examples][spaceflight][scene]")
{
    auto value = parse(R"(
mesh = "../meshes/ship.vmesh";
camera = { position = [11, 7, -10]; target = [0, 0, 0]; };
flight = { duration = 20; loop = false; bank_degrees = 7; };
background = { stars = 321; seed = 123; arbitrary_extra = true; };
bloom = { strength = 0.4; exposure = 1.7; };
custom_data = { owner = "editor"; arbitrary = [1, false, null]; };
)");
    REQUIRE(value);
    CHECK(value->mesh_path == "/example/meshes/ship.vmesh");
    CHECK(value->camera.position == vng::Vec3{11, 7, -10});
    CHECK(value->camera.vertical_fov_degrees == 38.0F);
    CHECK(value->flight.duration == 20.0F);
    CHECK_FALSE(value->flight.loop);
    CHECK(value->flight.bank_degrees == 7.0F);
    CHECK(value->star_count == 321);
    CHECK(value->star_seed == 123);
    CHECK(value->bloom.strength == 0.4F);
    CHECK(value->bloom.exposure == 1.7F);
    CHECK(value->extent == vng::Extent2D{1280, 800});

    auto minimal = parse("mesh = \"ship.vmesh\";");
    REQUIRE(minimal);
    CHECK(minimal->flight.start == vng::Vec3{0, 0, 40});
    CHECK(minimal->star_count == 700);
    CHECK(minimal->hero_time == 10.7F);
    auto absolute = parse("mesh = \"/assets/ship.vmesh\";");
    REQUIRE(absolute);
    CHECK(absolute->mesh_path == "/assets/ship.vmesh");
}

TEST_CASE("Spaceflight decoding diagnoses missing and invalid configuration",
    "[examples][spaceflight][scene]")
{
    const std::array invalid{
        "mesh = \"\";",
        "mesh = 12;",
        "mesh = \"a\"; flight = { duration = 0; };",
        "mesh = \"a\"; flight = { bank_degrees = 45; };",
        "mesh = \"a\"; flight = { start = [0, 0, 1]; end = [0, 0, 1]; };",
        "mesh = \"a\"; camera = { vertical_fov_degrees = 180; };",
        "mesh = \"a\"; camera = { near_plane = 10; far_plane = 1; };",
        "mesh = \"a\"; camera = { position = [0, 1, 0]; target = [0, 0, 0]; };",
        "mesh = \"a\"; background = { stars = 20001; };",
        "mesh = \"a\"; background = false;",
        "mesh = \"a\"; bloom = { strength = -1; };",
        "mesh = \"a\"; bloom = { exposure = \"bright\"; };",
        "mesh = \"a\"; extent = [0, 720];",
        "mesh = \"a\"; hero_time = 17;",
        "mesh = \"a\"; sun = false;",
        "mesh = \"a\"; sun = { radius = 0; };",
        "mesh = \"a\"; sun = { white_spots = 1; };",
        "mesh = \"a\"; flight = { controls = []; };",
        "mesh = \"a\"; flight = { controls = [[0,0,20]]; };",
        "mesh = \"a\"; flight = { controls = [[0,0,40],[0,0,10]]; };",
        "mesh = \"a\"; flight = { controls = [[0,0,10],[0,0,30]]; };",
    };
    for (auto source : invalid) {
        CAPTURE(source);
        auto result = parse(source);
        REQUIRE_FALSE(result);
        CHECK(result.error().path == "/example/scenes/flyby.vscene");
        CHECK(result.error().location.has_value());
        CHECK_FALSE(result.error().property_path.empty());
    }
    auto missing = parse("title = \"empty\";");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == content::ErrorCode::missing_property);
    auto malformed = parse("mesh = \"a\"; flight = { duration = \"fast\"; };");
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().property_path == "flight.duration");
    auto other_kind = content::parse_document("vasset 1.0\nmesh = \"a\";");
    REQUIRE(other_kind);
    CHECK_FALSE(scene::decode_scene(*other_kind));
}

TEST_CASE("Solar flyby stays clear of the sun and banks along a deterministic curve",
    "[examples][spaceflight][solar][timeline]")
{
    const auto value = scene::load_scene(VNG_TEST_SOLAR_FLYBY_PATH);
    REQUIRE(value);
    REQUIRE(value->sun);
    REQUIRE(value->flight.controls);
    CHECK_FALSE(value->sun->white_spots);
    CHECK_FALSE(value->flight.loop);
    CHECK(value->sun->position.x < 0.0F);
    CHECK(value->transform_at(0)[3].z > value->camera.position.z);
    CHECK(value->transform_at(0)[3].y < value->camera.position.y);
    CHECK(value->transform_at(0)[3].x < value->camera.position.x);
    const auto endpoint = value->flight.end;
    CHECK(value->transform_at(24)[3] == vng::Vec4{endpoint.x,endpoint.y,endpoint.z,1});
    CHECK(value->transform_at(30) == value->transform_at(24));
    for (float time = 0; time <= 24; time += 0.25F) {
        const auto matrix = value->transform_at(time);
        CHECK(matrix == value->transform_at(time));
        const auto p = matrix[3];
        const auto center = value->sun->position;
        const float distance = std::sqrt((p.x-center.x)*(p.x-center.x)
            + (p.y-center.y)*(p.y-center.y) + (p.z-center.z)*(p.z-center.z));
        CHECK(distance > value->sun->radius + 10);
        for (std::size_t a = 0; a < 3; ++a) {
            CHECK(matrix[a].w == 0.0F);
            for (std::size_t b = 0; b < 3; ++b) {
                float dot{};
                for (std::size_t i = 0; i < 3; ++i) dot += matrix[a][i]*matrix[b][i];
                CHECK(dot == Catch::Approx(a == b ? 1 : 0).margin(2e-6));
            }
        }
        if (time > 0 && time < 24) {
            const auto before = value->transform_at(time-0.001F)[3];
            const auto after = value->transform_at(time+0.001F)[3];
            const vng::Vec3 tangent{after.x-before.x,after.y-before.y,after.z-before.z};
            const float length = std::sqrt(tangent.x*tangent.x+tangent.y*tangent.y+tangent.z*tangent.z);
            const float alignment = -(matrix[2].x*tangent.x+matrix[2].y*tangent.y+matrix[2].z*tangent.z)/length;
            CHECK(alignment > 0.9999F);
        }
    }
    // Screen-space rightward movement after the below-camera entrance.
    float previous_x = -100;
    for (float time : {5.0F, 6.0F, 7.0F, 9.0F, 11.0F}) {
        const auto p = value->transform_at(time)[3];
        const auto projected_x = (p.x-value->camera.position.x) / (value->camera.position.z-p.z);
        CHECK(projected_x > previous_x);
        previous_x = projected_x;
    }
    const auto initial = value->transform_at(0)[3];
    const auto final = value->transform_at(24)[3];
    const auto center = value->sun->position;
    const float angle_start = std::atan2(initial.x-center.x, initial.z-center.z);
    const float angle_end = std::atan2(final.x-center.x, final.z-center.z);
    CHECK(angle_end-angle_start > 1.5F); // genuinely around the right flank
}

TEST_CASE("Spaceflight timeline is deterministic rigid and loops or clamps explicitly",
    "[examples][spaceflight][timeline]")
{
    scene::Scene value;
    const auto opening = value.transform_at(0);
    CHECK(opening[3] == vng::Vec4{0, 0, 40, 1});
    CHECK(value.transform_at(8)[3] == vng::Vec4{0, 0, 10, 1});
    CHECK(value.transform_at(16) == opening);
    CHECK(value.transform_at(-5) == opening);
    CHECK(value.transform_at(std::numeric_limits<float>::infinity()) == opening);
    CHECK(value.transform_at(24) == value.transform_at(8));
    for (float time : {0.0F, 1.0F, 8.0F, 10.7F, 15.9F}) {
        const auto matrix = value.transform_at(time);
        CHECK(matrix == value.transform_at(time));
        for (std::size_t a = 0; a < 3; ++a) {
            CHECK(matrix[a].w == 0.0F);
            for (std::size_t b = 0; b < 3; ++b) {
                float dot{};
                for (std::size_t axis = 0; axis < 3; ++axis) dot += matrix[a][axis] * matrix[b][axis];
                CHECK(dot == Catch::Approx(a == b ? 1.0F : 0.0F).margin(1.0e-6));
            }
        }
    }
    value.flight.loop = false;
    CHECK(value.transform_at(16)[3] == vng::Vec4{0, 0, -20, 1});
    CHECK(value.transform_at(30) == value.transform_at(16));
    value.flight.bank_degrees = 0;
    const auto unbanked = value.transform_at(8);
    CHECK(unbanked[0] == vng::Vec4{1, 0, 0, 0});
    CHECK(unbanked[1] == vng::Vec4{0, 1, 0, 0});
    CHECK(unbanked[2] == vng::Vec4{0, 0, 1, 0});
}

TEST_CASE("Spaceflight capture options retain explicit reproducible sample times",
    "[examples][spaceflight][options]")
{
    auto defaults = options({});
    REQUIRE(defaults);
    CHECK(defaults->scene_path == "default.vscene");
    CHECK_FALSE(defaults->fixed_time);
    CHECK_FALSE(defaults->frame_limit);
    auto capture = options({"custom.vscene", "--once", "--time", "10.7",
        "--analyze", "capture", "--screenshot", "frame.ppm", "--no-bloom"});
    REQUIRE(capture);
    CHECK(capture->scene_path == "custom.vscene");
    CHECK(capture->frame_limit == 1);
    CHECK(capture->fixed_time == 10.7F);
    CHECK(capture->analysis_directory == "capture");
    CHECK(capture->screenshot_path == "frame.ppm");
    CHECK_FALSE(capture->bloom);
    auto opening = options({"--once", "--time", "0"});
    REQUIRE(opening);
    CHECK(opening->fixed_time == 0.0F);
    auto help = options({"--help"});
    REQUIRE(help);
    CHECK(help->help);
    for (auto arguments : {
        std::initializer_list<const char*>{"--once", "--frames", "2"},
        {"--frames", "0"}, {"--frames", "-1"}, {"--frames", "2.5"},
        {"--time", "nan"}, {"--time", "inf"}, {"--time", "-1"},
        {"--time", "1junk"}, {"--time", "1", "--time", "2"},
        {"--time"}, {"--analyze"}, {"--analyze", "--once"},
        {"--screenshot", ""}, {"--unknown"}, {"a.vscene", "b.vscene"},
    }) {
        CHECK_FALSE(options(arguments));
    }
}
