#include "../../examples/editor/settings.hpp"
#include "../../examples/editor/delete_shortcut.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
using namespace editor_example;
using namespace vng::input;
namespace fs = std::filesystem;
struct Directory {
    fs::path path;
    Directory() {
        std::array<char, 40> pattern{};
        std::ranges::copy(std::string_view{"/tmp/vng-settings-test-XXXXXX"}, pattern.begin());
        REQUIRE(::mkdtemp(pattern.data()));
        path = pattern.data();
    }
    ~Directory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void write(const fs::path& path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary};
    REQUIRE(output);
    output << bytes;
    output.close();
    REQUIRE(output);
}
std::string read(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
} // namespace

TEST_CASE("Editor settings validate caps separately from simulation time", "[editor][settings]") {
    CHECK(validate_settings(Settings{}));
    CHECK(Settings{}.debug_fps == 10);
    CHECK(validate_settings({0, 1, 0, 1, 25}));
    CHECK(validate_settings({240, 240, 240, 60, 100}));
    for (const auto invalid : {Settings{241, 60, 0, 10, 100}, Settings{60, 0, 0, 10, 100},
                               Settings{60, 241, 0, 10, 100}, Settings{60, 60, 241, 10, 100},
                               Settings{60, 60, 0, 10, 24}, Settings{60, 60, 0, 10, 101}}) {
        const auto result = validate_settings(invalid);
        CHECK_FALSE(result);
        REQUIRE_FALSE(result.error().message.empty());
        CHECK(result.error().message.size() < 80);
    }
    CHECK(frame_interval(0) == std::chrono::steady_clock::duration::zero());
    const auto seconds = std::chrono::duration<double>(frame_interval(60)).count();
    CHECK(std::abs(seconds - 1.0 / 60) < .000001);
    CHECK(frame_interval(120) < frame_interval(60));
}

TEST_CASE("Debug preview FPS accepts the unsigned range with zero meaning unlimited",
          "[editor][settings][regression]") {
    Directory directory;
    const auto path = directory.path / "editor.settings";
    for (const auto fps : {0U, 1U, 61U, 144U, 360U, std::numeric_limits<unsigned>::max()}) {
        CAPTURE(fps);
        auto settings = Settings{};
        settings.debug_fps = fps;
        REQUIRE(validate_settings(settings));
        const auto decoded = decode_settings(encode_settings(settings));
        REQUIRE(decoded);
        CHECK(*decoded == settings);
        REQUIRE(save_settings(path, settings));
        const auto loaded = load_settings(path);
        REQUIRE(loaded);
        CHECK(*loaded == settings);
    }
    CHECK(frame_interval(0) == std::chrono::steady_clock::duration::zero());
    CHECK(frame_interval(360) < frame_interval(144));
    CHECK(frame_interval(std::numeric_limits<unsigned>::max()) >=
          std::chrono::steady_clock::duration::zero());
    // The settings transport uses unsigned parsing, not a signed conversion or
    // an unchecked narrowing cast: removing the rate cap must not weaken it.
    for (const auto& invalid : {std::string{"-1"}, std::string{"+1"}, std::string{"1.5"},
                                std::to_string(std::numeric_limits<unsigned>::max()) + "0"}) {
        CAPTURE(invalid);
        CHECK_FALSE(decode_settings("vng-editor-settings 1\n60 60 0 " + invalid + " 100\n"));
    }
}

TEST_CASE("Editor settings documents are bounded versioned and deterministic", "[editor][settings]") {
    const Settings settings{144, 90, 0, 30, 75};
    const auto bytes = encode_settings(settings);
    CHECK(bytes.starts_with("vng-editor-settings 8\n144 90 0 30 75\n"));
    const auto decoded = decode_settings(bytes);
    REQUIRE(decoded);
    CHECK(*decoded == settings);
    CHECK(encode_settings(*decoded) == bytes);
    CHECK(decode_settings("vng-editor-settings 1\n 60\r\n60 0 10 100 \n"));
    for (const auto invalid : {"", "vng-editor-settings 2\n60 60 0 10 100\n",
                               "vng-editor-settings 1\n60 60 0 10\n",
                               "vng-editor-settings 1\n60 60 0 10 100 20\n",
                               "vng-editor-settings 1\n60x 60 0 10 100\n",
                               "vng-editor-settings 1\n60.5 60 0 10 100\n",
                               "vng-editor-settings 1\n-1 60 0 10 100\n",
                               "vng-editor-settings 1\nnan 60 0 10 100\n",
                               "vng-editor-settings 1\n99999999999999999 60 0 10 100\n",
                               "vng-editor-settings 1\n60 0 0 10 100\n"})
        CHECK_FALSE(decode_settings(invalid));
    CHECK_FALSE(decode_settings(bytes + std::string(256, ' ')));
    CHECK_FALSE(decode_settings(bytes + std::string(1, '\0')));
}

TEST_CASE("Camera drag preferences persist and older settings opt into camera movement", "[editor][settings]") {
    Settings s; s.camera_drag={2,3,.6F}; s.scroll_moves_camera=false;
    auto decoded=decode_settings(encode_settings(s)); REQUIRE(decoded); CHECK(*decoded==s);
    auto legacy=decode_settings("vng-editor-settings 7\n60 60 0 10 100\n0.01 10000\n256\n4096\n1\n10000 10 10 10 4\n100\n");
    REQUIRE(legacy); CHECK(legacy->scroll_moves_camera); CHECK(legacy->camera_drag==CameraDragSpeeds{});
    for (auto bad : {0.F,-1.F,101.F,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        s.camera_drag={bad,1,.3F}; CHECK_FALSE(validate_settings(s));
        s.camera_drag={1,bad,.3F}; CHECK_FALSE(validate_settings(s));
        s.camera_drag={1,1,bad}; CHECK_FALSE(validate_settings(s));
    }
}

TEST_CASE("Viewing distance and walk preferences persist and validate", "[editor][settings][walk]") {
    Settings s; s.maximum_viewing_distance=75000; s.walk={12,8,5,3};
    auto decoded=decode_settings(encode_settings(s)); REQUIRE(decoded); CHECK(*decoded==s);
    auto legacy=decode_settings("vng-editor-settings 5\n60 60 0 10 100\n0.01 10000\n256\n4096\n1\n");
    REQUIRE(legacy); CHECK(legacy->maximum_viewing_distance==10000); CHECK(legacy->walk==WalkSpeeds{});
    for(float bad : {0.F,-1.F,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        auto invalid=s; invalid.maximum_viewing_distance=bad; CHECK_FALSE(validate_settings(invalid));
        invalid=s; invalid.walk.forward=bad; CHECK_FALSE(validate_settings(invalid));
        invalid=s; invalid.walk.sideways=bad; CHECK_FALSE(validate_settings(invalid));
        invalid=s; invalid.walk.vertical=bad; CHECK_FALSE(validate_settings(invalid));
        invalid=s; invalid.walk.fast_multiplier=bad; CHECK_FALSE(validate_settings(invalid));
    }
    auto bytes=encode_settings(s); bytes.pop_back(); bytes.pop_back();
    CHECK_FALSE(decode_settings(bytes));
}

TEST_CASE("UI scale persists and older settings retain 100 percent", "[editor][settings][ui-scale]") {
    Settings value; value.ui_scale_percent=125;
    auto decoded=decode_settings(encode_settings(value)); REQUIRE(decoded); CHECK(*decoded==value);
    auto legacy=decode_settings("vng-editor-settings 6\n60 60 0 10 100\n0.01 10000\n256\n4096\n1\n10000 10 10 10 4\n");
    REQUIRE(legacy); CHECK(legacy->ui_scale_percent==100);
    for(unsigned invalid:{0U,74U,151U}) { value.ui_scale_percent=invalid; CHECK_FALSE(validate_settings(value)); }
}

TEST_CASE("VSync preferences round trip and migrate independently of FPS caps", "[editor][settings][vsync]") {
    using vng::window::VSync;
    for(const auto mode:{VSync::off,VSync::on}) {
        Settings settings; settings.vsync=mode; settings.ui_fps=0;
        const auto decoded=decode_settings(encode_settings(settings)); REQUIRE(decoded);
        CHECK(*decoded==settings);
    }
    for(unsigned version=1;version<=4;++version) {
        auto bytes="vng-editor-settings "+std::to_string(version)+"\n60 60 0 10 100\n";
        if(version>=2) bytes+="0.01 10000\n";
        if(version>=3) bytes+="256\n";
        if(version>=4) bytes+="4096\n";
        auto decoded=decode_settings(bytes); REQUIRE(decoded);
        CHECK(decoded->vsync==VSync::on);
    }
    const std::string prefix="vng-editor-settings 5\n60 60 0 10 100\n0.01 10000\n256\n4096\n";
    for(const auto suffix:{"","-1","2","1.0","nan","1extra","0 1"})
        CHECK_FALSE(decode_settings(prefix+suffix));
    auto invalid=Settings{}; invalid.vsync=static_cast<VSync>(5);
    CHECK_FALSE(validate_settings(invalid));
}

TEST_CASE("Timeline track preferences round trip and migrate without changing older preferences", "[editor][settings][timeline]") {
    Directory directory;
    const auto path = directory.path / "editor.settings";
    Settings settings;
    for (auto limit : {1U, 256U, 1024U, static_cast<unsigned>(vng::timeline::max_tracks)}) {
        settings.timeline_track_limit = limit;
        REQUIRE(validate_settings(settings));
        REQUIRE(save_settings(path, settings));
        const auto loaded = load_settings(path); REQUIRE(loaded);
        CHECK(*loaded == settings);
    }
    for (auto version : {1, 2}) {
        auto old = decode_settings("vng-editor-settings " + std::to_string(version) +
            "\n120 90 45 144 75\n" + (version == 2 ? "0.005 25000\n" : ""));
        REQUIRE(old);
        CHECK(old->timeline_track_limit == 256);
        CHECK(old->ui_fps == 120); CHECK(old->debug_fps == 144);
        if (version == 2) CHECK(old->orbit_distance == OrbitDistanceRange{.005F, 25000});
    }
    for (const auto text : {"", "0", "-1", "1.5", "nan", "16385", "42949672960", "256extra", "256 1"}) {
        CHECK_FALSE(decode_settings(std::string{"vng-editor-settings 3\n60 60 0 10 100\n0.01 10000\n"} + text));
    }
    const auto saved = read(path);
    for (auto value : {0U, static_cast<unsigned>(vng::timeline::max_tracks + 1)}) {
        settings.timeline_track_limit = value;
        CHECK_FALSE(save_settings(path, settings));
        CHECK(read(path) == saved);
    }
}

TEST_CASE("Instance preferences migrate from old settings and reject invalid limits", "[editor][settings][instances]") {
    CHECK(Settings{}.instance_limit==4096);
    for (unsigned limit : {1U, 300U, 8192U, max_scene_instances}) {
        Settings s; s.instance_limit=limit;
        auto restored=decode_settings(encode_settings(s)); REQUIRE(restored);
        CHECK(*restored==s);
    }
    for (const auto version : {1,2,3}) {
        auto restored=decode_settings("vng-editor-settings "+std::to_string(version)+
            "\n60 60 0 10 100\n"+(version>=2 ? "0.01 10000\n" : "")+(version>=3 ? "1024\n" : ""));
        REQUIRE(restored); CHECK(restored->instance_limit==4096);
    }
    for(const auto value : {"", "0", "-1", "1.5", "65537", "4096extra", "4096 2"})
        CHECK_FALSE(decode_settings(std::string{"vng-editor-settings 4\n60 60 0 10 100\n0.01 10000\n256\n"}+value));
}

TEST_CASE("Orbit-distance preferences round trip and old preferences inherit the wider range", "[editor][settings][orbit_distance]") {
    Settings s;
    s.orbit_distance = {.0025F, 25000};
    REQUIRE(validate_settings(s));
    auto decoded = decode_settings(encode_settings(s)); REQUIRE(decoded);
    CHECK(*decoded == s);
    auto legacy = decode_settings("vng-editor-settings 1\n60 60 0 10 100\n"); REQUIRE(legacy);
    CHECK(legacy->orbit_distance == OrbitDistanceRange{});
    for (auto range : {OrbitDistanceRange{0,100}, OrbitDistanceRange{2,1}, OrbitDistanceRange{2,2},
         OrbitDistanceRange{.01F,1'000'001.F}, OrbitDistanceRange{std::numeric_limits<float>::quiet_NaN(),100}}) {
        s.orbit_distance = range; CHECK_FALSE(validate_settings(s)); CHECK_FALSE(decode_settings(encode_settings(s)));
    }
}

TEST_CASE("Editor settings persist atomically without creating scenes", "[editor][settings][file]") {
    Directory directory;
    const auto path = directory.path / "nested/editor.settings";
    auto loaded = load_settings(path);
    REQUIRE(loaded);
    CHECK(*loaded == Settings{});
    CHECK_FALSE(fs::exists(path));
    Settings settings{120, 45, 0, 15, 100};
    REQUIRE(save_settings(path, settings));
    loaded = load_settings(path);
    REQUIRE(loaded);
    CHECK(*loaded == settings);
    CHECK(read(path) == encode_settings(settings));
    settings.preview_percent = 75;
    REQUIRE(save_settings(path, settings));
    CHECK(read(path) == encode_settings(settings));
    auto invalid = settings;
    invalid.preview_fps = 0;
    CHECK_FALSE(save_settings(path, invalid));
    CHECK(read(path) == encode_settings(settings));
    for (const auto& entry : fs::recursive_directory_iterator(directory.path))
        CHECK_FALSE(entry.path().filename().string().starts_with(".editor-settings-"));
    write(path, "invalid settings");
    CHECK_FALSE(load_settings(path));
    CHECK(read(path) == "invalid settings");
    write(path, std::string(257, 'x'));
    CHECK_FALSE(load_settings(path));
    CHECK_FALSE(load_settings(directory.path));
    CHECK_FALSE(save_settings(directory.path, settings));
    const auto link = directory.path / "link.settings";
    fs::create_symlink(path, link);
    CHECK_FALSE(save_settings(link, settings));
    CHECK(fs::is_symlink(link));
    CHECK(read(path) == std::string(257, 'x'));
    CHECK_FALSE(save_settings(path / "child", settings)); // Existing parent is a regular file.
}

TEST_CASE("Editor Delete shortcuts require a fresh unhandled key in stable context",
          "[editor][input][delete]") {
    const Event press{.kind = EventKind::key_down, .key = Key::del};
    const std::array raw{press};
    CHECK(delete_pressed(raw, raw, true));
    CHECK_FALSE(delete_pressed(raw, raw, false));
    CHECK_FALSE(delete_pressed({}, raw, true)); // Text consumed Delete in the UI.
    for (const auto& event : {
             Event{.kind = EventKind::key_up, .key = Key::del},
             Event{.kind = EventKind::key_down, .key = Key::del, .repeat = true},
             Event{.kind = EventKind::key_down, .key = Key::backspace},
             Event{.kind = EventKind::text, .key = Key::del},
             Event{.kind = EventKind::key_down, .key = Key::del, .modifiers = {.control = true}},
             Event{.kind = EventKind::key_down, .key = Key::del, .modifiers = {.shift = true}},
             Event{.kind = EventKind::key_down, .key = Key::del, .modifiers = {.alt = true}},
             Event{.kind = EventKind::key_down, .key = Key::del, .modifiers = {.super = true}}}) {
        const std::array events{event};
        CHECK_FALSE(delete_pressed(events, events, true));
    }
    for (const auto& event : {Event{.kind = EventKind::focus_lost},
                             Event{.kind = EventKind::pointer_down, .button = 0}}) {
        const std::array before{event, press}, after{press, event};
        CHECK_FALSE(delete_pressed(raw, before, true));
        CHECK_FALSE(delete_pressed(raw, after, true));
    }
    const std::array movement{Event{.kind = EventKind::pointer_move, .position = {10, 20}}, press};
    CHECK(delete_pressed(raw, movement, true));
}
