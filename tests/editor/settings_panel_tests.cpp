#include "../../examples/editor/settings_panel.hpp"
#include "../../examples/editor/camera_preferences.hpp"
#include "../../examples/editor/delete_shortcut.hpp"
#include <vng/ui/inspection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <limits>

namespace {
using namespace vng;
using input::EventKind;
using input::Key;
constexpr std::array labels{"UI FPS cap (0 = unlimited)", "Embedded preview FPS cap",
                            "Independent Play FPS cap (0 = unlimited)",
                            "Debug-preview FPS cap (0 = unlimited)", "Preview resolution % (100 = native)",
                            "Minimum orbit distance", "Maximum orbit distance", "Timeline track limit", "Instance limit",
                            "Maximum viewing distance", "Walk forward speed (units/s)", "Walk sideways speed (units/s)",
                            "Walk vertical speed (units/s)", "Walk Shift multiplier"};
text::Font font() {
    auto result = text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(result);
    return *result;
}
struct Fixture {
    ui::Screen screen{ui::dark_theme(font())};
    ui::Container host = screen.column().position({100, 0}).width(760).height(900);
    editor_example::SettingsPanel panel{host};
    input::Frame input{.logical_size = {1000, 1000}, .framebuffer = {1000, 1000}};
    ui::UpdateResult update;
    void pump(std::initializer_list<input::Event> events = {}) {
        input.events = events;
        for (const auto& event : events)
            if (event.kind == EventKind::pointer_down || event.kind == EventKind::pointer_up ||
                event.kind == EventKind::pointer_move)
                input.pointer = event.position;
        const auto result = screen.update(input, .016F);
        REQUIRE(result);
        update = *result;
    }
    auto poll() { return panel.poll(input.events); }
    ui::TextDraw find(std::string_view text) {
        const auto draws = screen.draw_list();
        REQUIRE(draws);
        for (const auto& command : draws->commands)
            if (const auto* value = std::get_if<ui::TextDraw>(&command); value && value->text == text)
                return *value;
        FAIL("No settings label: " << text);
        return {};
    }
    bool has(std::string_view text) {
        const auto draws = screen.draw_list();
        REQUIRE(draws);
        return std::ranges::any_of(draws->commands, [&](const auto& command) {
            const auto* value = std::get_if<ui::TextDraw>(&command);
            return value && value->text == text;
        });
    }
    void click(Vec2 point) {
        pump({{.kind = EventKind::pointer_down, .position = point},
              {.kind = EventKind::pointer_up, .position = point}});
    }
    void click(std::string_view caption) {
        const auto value = find(caption);
        click({value.position.x + 3, value.position.y + 8});
    }
    void enter(std::size_t index, std::string replacement, bool submit = false) {
        const auto label = find(labels[index]);
        click({label.clip.x + label.clip.width + 18, label.position.y + 8});
        const input::Event select{
            .kind = EventKind::key_down, .key = Key::a, .modifiers = {.control = true}};
        const input::Event text{.kind = EventKind::text, .text = std::move(replacement)};
        if (submit)
            pump({select, text, {.kind = EventKind::key_down, .key = Key::enter}});
        else
            pump({select, text});
    }
    void labels_fit() {
        const auto draws = screen.draw_list();
        REQUIRE(draws);
        for (const auto& command : draws->commands)
            if (const auto* value = std::get_if<ui::TextDraw>(&command)) {
                // Numeric inputs deliberately scroll long drafts horizontally;
                // captions, instructions, status messages and buttons must fit.
                if (value->clip.width < 100)
                    continue;
                INFO(value->text);
                const auto metrics = value->font.measure(value->text, static_cast<u32>(value->size));
                REQUIRE(metrics);
                CHECK(value->position.x >= value->clip.x);
                CHECK(value->position.x + metrics->width <=
                      value->clip.x + value->clip.width + .01F);
                CHECK(value->position.y >= value->clip.y);
                CHECK(value->position.y + value->size <= value->clip.y + value->clip.height);
            }
    }
};
} // namespace

TEST_CASE("Settings panel stages typed values and only emits validated Apply or fresh Enter",
          "[editor][ui][settings]") {
    Fixture f;
    f.pump();
    CHECK_FALSE(f.panel.visible());
    CHECK_FALSE(f.poll());
    f.panel.open({});
    f.pump();
    REQUIRE(f.panel.visible());
    f.labels_fit();
    f.enter(0, "144");
    CHECK_FALSE(f.poll());
    f.enter(1, " 90 ");
    CHECK_FALSE(f.poll());
    f.enter(2, "120");
    f.enter(3, "15");
    f.enter(4, "75", true);
    CHECK_FALSE(f.panel.poll({})); // Raw key is required for keyboard consent.
    auto settings = f.poll();
    REQUIRE(settings);
    CHECK(*settings == editor_example::Settings{144, 90, 120, 15, 75});
    CHECK(f.panel.visible()); // The host persists before closing.
    f.pump({{.kind = EventKind::key_down, .key = Key::enter, .repeat = true}});
    CHECK_FALSE(f.poll());
    f.pump();
    CHECK_FALSE(f.poll());
    f.click("Apply & save");
    settings = f.poll();
    REQUIRE(settings);
    CHECK(*settings == editor_example::Settings{144, 90, 120, 15, 75});
    f.panel.close();
    f.pump();
    CHECK_FALSE(f.panel.visible());
    CHECK_FALSE(f.has("EDITOR SETTINGS"));
    CHECK_FALSE(f.poll());
}

TEST_CASE("Settings panel accepts fractional orbit_distance and rejects reversed ranges", "[editor][ui][settings][orbit_distance]") {
    Fixture f; f.panel.open({}); f.pump(); f.labels_fit();
    f.enter(5,"0.005"); f.enter(6,"25000",true);
    auto settings = f.poll(); REQUIRE(settings);
    CHECK(settings->orbit_distance == editor_example::OrbitDistanceRange{.005F,25000});
    f.enter(6,"0.001",true); CHECK_FALSE(f.poll()); f.labels_fit();
    f.enter(6,"nan",true); CHECK_FALSE(f.poll());
}

TEST_CASE("Settings panel exposes viewing distance and independent walk speeds", "[editor][ui][settings][walk]") {
    Fixture f; f.panel.open({}); f.pump(); f.labels_fit();
    f.enter(9,"50000"); f.enter(10,"12"); f.enter(11,"8"); f.enter(12,"5"); f.enter(13,"3",true);
    auto settings=f.poll(); REQUIRE(settings);
    CHECK(settings->maximum_viewing_distance==50000);
    CHECK(settings->walk==editor_example::WalkSpeeds{12,8,5,3});
    f.enter(9,"nan",true); CHECK_FALSE(f.poll());
}

TEST_CASE("Camera preference drafts preserve unrelated editor preferences", "[editor][ui][camera]") {
    ui::Screen screen{ui::dark_theme(font())};
    editor_example::CameraPreferences panel{screen.column()};
    editor_example::Settings settings; settings.ui_scale_percent=125; settings.maximum_viewing_distance=25000;
    settings.walk={15,8,5,3}; settings.camera_drag={2,4,.7F}; settings.scroll_moves_camera=false; panel.show(settings);
    auto edited=panel.read(settings); REQUIRE(edited); CHECK(*edited==settings);
    CHECK_FALSE(panel.applied());
}
TEST_CASE("Walk speed sliders update the preference draft before explicit save", "[editor][ui][camera]") {
    ui::Screen screen{ui::dark_theme(font())};
    editor_example::CameraPreferences panel{screen.column().width(600)};
    editor_example::Settings settings;panel.show(settings);
    input::Frame frame{.logical_size={800,1000},.framebuffer={800,1000}};
    const auto pump=[&] {REQUIRE(screen.update(frame,.016F));panel.poll();};
    pump();pump();
    const auto snapshot=screen.inspect();REQUIRE(snapshot);
    const auto slider=std::ranges::find_if(snapshot->widgets,[](const auto& item) {
        return item.role==ui::WidgetRole::slider && item.label=="Walk forward";
    });
    REQUIRE(slider!=snapshot->widgets.end());
    const Vec2 p{slider->bounds.x+slider->bounds.width*.75F,slider->bounds.y+slider->bounds.height*.5F};
    frame.pointer=p;
    frame.events={{.kind=EventKind::pointer_down,.position=p},{.kind=EventKind::pointer_up,.position=p}};pump();
    const auto draft=panel.read(settings);REQUIRE(draft);
    CHECK(draft->walk.forward>50.F);CHECK(draft->walk.forward<100.F);
    CHECK(draft->walk.sideways==settings.walk.sideways);
    CHECK_FALSE(panel.applied());
}

TEST_CASE("General settings preserve camera preferences not edited by this panel", "[editor][ui][settings]") {
    Fixture f;
    editor_example::Settings settings; settings.camera_drag={2,3,.6F}; settings.scroll_moves_camera=false;
    f.panel.open(settings); f.pump(); f.enter(0,"144",true);
    auto edited=f.poll(); REQUIRE(edited);
    CHECK(edited->camera_drag==settings.camera_drag); CHECK_FALSE(edited->scroll_moves_camera);
}

TEST_CASE("Settings panel exposes track usage and a validated track limit", "[editor][ui][settings][timeline]") {
    Fixture f; f.panel.open({}, 264); f.pump(); f.labels_fit();
    CHECK(f.has("0 instances / 264 tracks. Limits affect additions only."));
    f.enter(7, " 1024 ", true);
    auto settings = f.poll(); REQUIRE(settings);
    CHECK(settings->timeline_track_limit == 1024);
    f.panel.close(); f.panel.open(*settings, 264); f.pump();
    f.click("Apply & save"); auto reapplied = f.poll(); REQUIRE(reapplied);
    CHECK(reapplied->timeline_track_limit == 1024);
    for (const auto value : {"", "0", "-1", "1.5", "nan", "16385", "42949672960"}) {
        f.enter(7, value, true); CHECK_FALSE(f.poll()); f.labels_fit();
    }
    f.enter(7, "1", true); REQUIRE(f.poll()); // Lowering never deletes existing tracks.
}

TEST_CASE("Instance limit is editable in settings", "[editor][ui][settings][instances]") {
    Fixture f; f.panel.open({}, 640, 641); f.pump(); f.labels_fit();
    CHECK(f.has("641 instances / 640 tracks. Limits affect additions only."));
    f.enter(8,"8192",true);
    auto settings=f.poll(); REQUIRE(settings); CHECK(settings->instance_limit==8192);
    for(const auto value : {"0", "65537", "-1", "3.5"}) {
        f.enter(8,value,true); CHECK_FALSE(f.poll());
    }
    f.enter(8,"1",true); REQUIRE(f.poll());
}

TEST_CASE("Settings VSync checkbox is staged, reapplied and reset by Defaults", "[editor][ui][settings][vsync]") {
    Fixture f; f.panel.open({}); f.pump(); f.labels_fit();
    f.click("VSync (editor and independent Play)");
    CHECK_FALSE(f.poll()); // No implicit application while editing a preference.
    f.click("Apply & save");
    auto next=f.poll(); REQUIRE(next); CHECK(next->vsync==window::VSync::off);
    f.panel.close(); f.panel.open(*next); f.pump();
    f.click("Apply & save");
    auto reapplied=f.poll(); REQUIRE(reapplied); CHECK(reapplied->vsync==window::VSync::off);
    f.click("Defaults"); CHECK_FALSE(f.poll());
    f.click("Apply & save");
    auto defaults=f.poll(); REQUIRE(defaults); CHECK(defaults->vsync==window::VSync::on);
    f.labels_fit();
}

TEST_CASE("VSync and Apply remain reachable in a short Settings panel", "[editor][ui][settings][vsync]") {
    Fixture f; f.host.height(400); f.panel.open({}); f.pump();
    REQUIRE(f.host.scroll_limit()>0);
    f.host.scroll(f.host.scroll_limit()); f.pump();
    for(const auto caption:{"VSync (editor and independent Play)","Apply & save"}) {
        const auto text=f.find(caption);
        CHECK(text.position.y>=text.clip.y);
        CHECK(text.position.y+text.size<=text.clip.y+text.clip.height);
    }
    f.click("VSync (editor and independent Play)"); CHECK_FALSE(f.poll());
    f.click("Apply & save"); auto next=f.poll(); REQUIRE(next);
    CHECK(next->vsync==window::VSync::off);
}

TEST_CASE("Settings panel rejects bad numbers and displays unclipped range errors",
          "[editor][ui][settings]") {
    Fixture f;
    f.panel.open({});
    f.pump();
    for (const auto text : {"", "nan", "-1", "1.5", "1e2", "42949672960"}) {
        f.enter(0, text, true);
        CHECK_FALSE(f.poll());
        CHECK(f.has("Enter whole numbers in all settings fields."));
        f.labels_fit();
    }
    for (const std::size_t i : {0U, 1U, 2U, 4U}) {
        f.panel.open({});
        f.pump();
        f.enter(i, i == 4 ? "101" : "241", true);
        CHECK_FALSE(f.poll());
        f.labels_fit();
    }
    f.panel.open({});
    f.pump();
    f.enter(1, "0", true);
    CHECK_FALSE(f.poll());
    CHECK(f.has("Embedded preview FPS cap must be 1..240."));
    f.enter(1, "1", true);
    REQUIRE(f.poll());
    f.enter(4, "24", true);
    CHECK_FALSE(f.poll());
    f.enter(4, "25", true);
    REQUIRE(f.poll());
}

TEST_CASE("Settings panel accepts unlimited and high debug-preview rates without another cap",
          "[editor][ui][settings][regression]") {
    Fixture f;
    for (const auto fps : {144U, 360U, 0U, std::numeric_limits<unsigned>::max()}) {
        CAPTURE(fps);
        f.panel.open({});
        f.pump();
        f.labels_fit();
        f.enter(3, std::to_string(fps), true);
        const auto applied = f.poll();
        REQUIRE(applied);
        CHECK(applied->debug_fps == fps);
        const auto decoded = editor_example::decode_settings(editor_example::encode_settings(*applied));
        REQUIRE(decoded);
        CHECK(*decoded == *applied);
        f.panel.close();
        f.panel.open(*decoded);
        f.pump();
        f.click("Apply & save");
        const auto reapplied = f.poll();
        REQUIRE(reapplied);
        CHECK(*reapplied == *applied);
    }
    // The debug field remains an unsigned whole-number input, including when
    // the old rate ceiling is absent.
    for (const auto& invalid : {std::string{"-1"}, std::string{"1.5"}, std::string{"nan"},
                                std::to_string(std::numeric_limits<unsigned>::max()) + "0"}) {
        f.enter(3, invalid, true);
        CHECK_FALSE(f.poll());
        CHECK(f.has("Enter whole numbers in all settings fields."));
        f.labels_fit();
    }
}

TEST_CASE("Settings panel Cancel Escape and Defaults do not silently apply preferences",
          "[editor][ui][settings]") {
    Fixture f;
    const editor_example::Settings saved{120, 90, 45, 20, 75};
    f.panel.open(saved);
    f.pump();
    f.enter(0, "144");
    CHECK_FALSE(f.poll());
    f.click("Cancel");
    CHECK_FALSE(f.poll());
    CHECK_FALSE(f.panel.visible());
    f.panel.open(saved);
    f.pump();
    CHECK(f.has("120"));
    f.enter(0, "144");
    f.pump({{.kind = EventKind::key_down, .key = Key::escape}});
    CHECK(f.update.unhandled().empty()); // Escape was consumed by the focused field.
    CHECK_FALSE(f.poll());
    CHECK_FALSE(f.panel.visible());
    f.panel.open(saved);
    f.pump();
    f.click("Defaults");
    CHECK_FALSE(f.poll());
    CHECK(f.panel.visible());
    CHECK(f.has("Defaults staged; Apply to save."));
    f.click("Apply & save");
    const auto defaults = f.poll();
    REQUIRE(defaults);
    CHECK(*defaults == editor_example::Settings{});
}

TEST_CASE("Settings fields keep Delete inside text editing and modal shortcuts fail closed",
          "[editor][ui][settings][delete]") {
    Fixture f;
    f.panel.open({});
    f.pump();
    f.enter(0, "144");
    f.pump({{.kind = EventKind::key_down, .key = Key::a, .modifiers = {.control = true}},
            {.kind = EventKind::key_down, .key = Key::del}});
    CHECK(f.update.unhandled().empty());
    CHECK_FALSE(editor_example::delete_pressed(f.update.unhandled(), f.input.events, true));
    f.click("Apply & save");
    CHECK_FALSE(f.poll());
    CHECK(f.has("Enter whole numbers in all settings fields."));
    f.pump({{.kind = EventKind::key_down, .key = Key::del}});
    REQUIRE(f.update.unhandled().size() == 1); // Button focus can bubble Delete.
    CHECK_FALSE(editor_example::delete_pressed(f.update.unhandled(), f.input.events,
                                               !f.panel.visible()));
}
