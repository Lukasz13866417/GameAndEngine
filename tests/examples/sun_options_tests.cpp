#include "support/sun_options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <vector>

namespace {

namespace sun = example::sun;

vng::resources::Result<sun::Options> options(std::initializer_list<const char*> arguments)
{
    std::vector<std::string> strings{"sun"};
    for (const auto* argument : arguments) strings.emplace_back(argument);
    std::vector<char*> argv;
    for (auto& string : strings) argv.push_back(string.data());
    return sun::parse_options(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST_CASE("Sun demo defaults to animation with white spots disabled",
    "[examples][sun][options]")
{
    auto value = options({});
    REQUIRE(value);
    CHECK(value->extent == vng::Extent2D{1280, 800});
    CHECK_FALSE(value->frame_limit);
    CHECK_FALSE(value->fixed_time);
    CHECK_FALSE(value->analysis_directory);
    CHECK_FALSE(value->screenshot_path);
    CHECK(value->bloom);
    CHECK(value->displacement);
    CHECK_FALSE(value->white_spots);
    CHECK_FALSE(value->help);

    auto once = options({"--once"});
    REQUIRE(once);
    CHECK(once->frame_limit == 1);
    CHECK_FALSE(once->fixed_time);
    for (const auto* flag : {"--help", "-h"}) {
        auto help = options({flag});
        REQUIRE(help);
        CHECK(help->help);
    }
}

TEST_CASE("Sun captures are reproducible and honor explicit user settings",
    "[examples][sun][options]")
{
    for (const auto* flag : {"--screenshot", "--analyze"}) {
        auto capture = options({flag, "output"});
        REQUIRE(capture);
        CHECK(capture->fixed_time == 3.0F);
        CHECK(capture->frame_limit == 1);
    }
    auto explicit_settings = options({"--screenshot", "sun.png", "--analyze", "evidence",
        "--frames", "5", "--time", "0", "--no-bloom", "--no-displacement", "--no-white-spots",
        "--width", "640", "--height", "480"});
    REQUIRE(explicit_settings);
    CHECK(explicit_settings->screenshot_path == "sun.png");
    CHECK(explicit_settings->analysis_directory == "evidence");
    CHECK(explicit_settings->frame_limit == 5);
    CHECK(explicit_settings->fixed_time == 0.0F);
    CHECK_FALSE(explicit_settings->bloom);
    CHECK_FALSE(explicit_settings->displacement);
    CHECK_FALSE(explicit_settings->white_spots);
    CHECK(explicit_settings->extent == vng::Extent2D{640, 480});

    auto time_only = options({"--time", "1.25"});
    REQUIRE(time_only);
    CHECK(time_only->fixed_time == 1.25F);
    CHECK_FALSE(time_only->frame_limit);
    auto spots_off = options({"--no-white-spots"});
    REQUIRE(spots_off);
    CHECK_FALSE(spots_off->white_spots);
    CHECK(spots_off->bloom);
    CHECK(spots_off->displacement);
    CHECK_FALSE(spots_off->frame_limit);
    auto spots_on = options({"--white-spots"});
    REQUIRE(spots_on);
    CHECK(spots_on->white_spots);
    auto once_capture = options({"--once", "--analyze", "evidence"});
    REQUIRE(once_capture);
    CHECK(once_capture->frame_limit == 1);
    CHECK(once_capture->fixed_time == 3.0F);
}

TEST_CASE("Sun options reject malformed ambiguous and nonfinite values",
    "[examples][sun][options]")
{
    for (auto arguments : {
        std::initializer_list<const char*>{"--once", "--frames", "2"},
        {"--once", "--once"}, {"--frames", "0"}, {"--frames", "-1"},
        {"--frames", "1.5"}, {"--frames", "18446744073709551616"},
        {"--time", "nan"}, {"--time", "inf"}, {"--time", "-1"},
        {"--time", "1e100"}, {"--time", "1junk"}, {"--time", "1", "--time", "2"},
        {"--time"}, {"--frames"}, {"--analyze"}, {"--screenshot"},
        {"--analyze", "--once"}, {"--screenshot", ""},
        {"--screenshot", "a.png", "--screenshot", "b.png"},
        {"--analyze", "a", "--analyze", "b"},
        {"--width"}, {"--height"}, {"--width", "63"}, {"--height", "8193"},
        {"--width", "-640"}, {"--height", "640.5"},
        {"--width", "640", "--width", "800"},
        {"--height", "640", "--height", "800"},
        {"--unknown"}, {"sun.vscene"},
    }) {
        auto value = options(arguments);
        REQUIRE_FALSE(value);
        CHECK(value.error().code == vng::resources::ErrorCode::invalid_argument);
        CHECK_FALSE(value.error().message.empty());
        CHECK_FALSE(value.error().context.empty());
    }
    CHECK_FALSE(sun::parse_options(0, nullptr));
    CHECK_FALSE(sun::parse_options(1, nullptr));
    char name[] = "sun";
    char* null_name[]{nullptr};
    CHECK_FALSE(sun::parse_options(1, null_name));
    char* null_argument[]{name, nullptr};
    CHECK_FALSE(sun::parse_options(2, null_argument));
}
