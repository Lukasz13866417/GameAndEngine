#include "../../examples/editor/save_dialog.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>

namespace {
using namespace vng;
using input::EventKind;
using input::Key;
text::Font font() {
    auto value = text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(value);
    return *value;
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        std::array<char, 31> pattern{};
        constexpr std::string_view text = "/tmp/vng-save-dialog-XXXXXX";
        std::ranges::copy(text, pattern.begin());
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        path = pattern.data();
    }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path existing(std::string_view name) const {
        const auto file = path / name;
        std::ofstream output(file);
        REQUIRE(output);
        output << "Original scene; the dialog must never write this file.";
        return file;
    }
};
struct Fixture {
    ui::Screen screen{ui::dark_theme(font())};
    ui::Container host = screen.column().position({100, 100}).width(620).height(230);
    editor_example::SaveSceneDialog dialog{host};
    input::Frame input{.logical_size = {1000, 700}, .framebuffer = {1000, 700}};
    bool keyboard{};
    void pump(std::initializer_list<input::Event> events = {}) {
        input.events = events;
        for (const auto& event : events)
            if (event.kind == EventKind::pointer_down || event.kind == EventKind::pointer_up ||
                event.kind == EventKind::pointer_move)
                input.pointer = event.position;
        auto result = screen.update(input, .016F);
        REQUIRE(result);
        keyboard = result->capturesKeyboard;
    }
    std::optional<editor_example::SaveRequest> poll() { return dialog.poll(input.events); }
    bool has(std::string_view text) {
        const auto list = screen.draw_list();
        REQUIRE(list);
        return std::ranges::any_of(list->commands, [&](const auto& command) {
            const auto* draw = std::get_if<ui::TextDraw>(&command);
            return draw && draw->text == text;
        });
    }
    Vec2 at(std::string_view text) {
        const auto list = screen.draw_list();
        REQUIRE(list);
        for (const auto& command : list->commands)
            if (const auto* draw = std::get_if<ui::TextDraw>(&command); draw && draw->text == text)
                return {draw->position.x + 3, draw->position.y + 8};
        FAIL("No save-dialog text: " << text);
        return {};
    }
    void click(std::string_view text) {
        const auto position = at(text);
        pump({{.kind = EventKind::pointer_down, .position = position},
              {.kind = EventKind::pointer_up, .position = position}});
    }
    void filename(std::string_view current, std::string next, bool enter = false) {
        click(current);
        const input::Event all{
            .kind = EventKind::key_down, .key = Key::a, .modifiers = {.control = true}};
        const input::Event text{.kind = EventKind::text, .text = std::move(next)};
        if (enter)
            pump({all, text, {.kind = EventKind::key_down, .key = Key::enter}});
        else
            pump({all, text});
    }
};
} // namespace

TEST_CASE("Save scene dialog focuses its filename and Enter returns a non-writing request",
          "[editor][ui][save-dialog]") {
    Directory directory;
    const auto path = directory.path / "fresh.vscene";
    Fixture f;
    CHECK_FALSE(f.dialog.visible());
    f.pump();
    CHECK_FALSE(f.has("Save scene as"));
    f.dialog.open(path);
    f.pump();
    REQUIRE(f.dialog.visible());
    CHECK(f.keyboard);
    CHECK(f.has(path.string()));
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    CHECK_FALSE(f.dialog.poll()); // Missing raw input must fail closed for keyboard consent.
    const auto request = f.poll();
    REQUIRE(request);
    CHECK(request->path == path);
    CHECK_FALSE(request->replace_existing);
    CHECK(f.dialog.visible()); // Host closes only after a successful write.
    CHECK_FALSE(std::filesystem::exists(path));
    f.dialog.close();
    f.pump();
    CHECK_FALSE(f.dialog.visible());
    CHECK_FALSE(f.has("Save scene as"));
    CHECK_FALSE(f.keyboard);
    CHECK_FALSE(f.poll());
}

TEST_CASE("Save scene dialog cancels by button or raw Escape even with text focus",
          "[editor][ui][save-dialog]") {
    Fixture f;
    f.pump();
    f.dialog.open("draft.vscene");
    f.pump();
    f.click("Cancel");
    CHECK_FALSE(f.poll());
    CHECK_FALSE(f.dialog.visible());
    f.dialog.open("second.vscene");
    f.pump();
    REQUIRE(f.keyboard);
    f.pump({{.kind = EventKind::key_down, .key = Key::escape}});
    CHECK_FALSE(f.poll());
    CHECK_FALSE(f.dialog.visible());
}

TEST_CASE("Save scene dialog explicitly confirms replacement without writing the target",
          "[editor][ui][save-dialog]") {
    Directory directory;
    const auto path = directory.existing("existing.vscene");
    const auto original_size = std::filesystem::file_size(path);
    Fixture f;
    f.pump();
    f.dialog.open(path);
    f.pump();
    f.click("Save");
    CHECK_FALSE(f.poll());
    CHECK(f.has("File exists. Replace it?"));
    CHECK(f.has("Replace"));
    REQUIRE(f.dialog.visible());
    f.click("Replace");
    const auto request = f.poll();
    REQUIRE(request);
    CHECK(request->path == path);
    CHECK(request->replace_existing);
    CHECK(std::filesystem::file_size(path) == original_size);
    f.dialog.error("Replacement failed.");
    CHECK(f.dialog.visible());
    CHECK(f.has("Replacement failed."));
    CHECK(f.has("Save"));
    CHECK_FALSE(f.has("Replace"));
}

TEST_CASE("Changing a save filename resets replacement confirmation including same-frame Enter",
          "[editor][ui][save-dialog]") {
    Directory directory;
    const auto first = directory.existing("first.vscene");
    const auto second = directory.existing("second.vscene");
    Fixture f;
    f.pump();
    f.dialog.open(first);
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    CHECK_FALSE(f.poll());
    REQUIRE(f.has("Replace"));
    f.filename(first.string(), second.string(), true);
    CHECK_FALSE(f.poll());
    CHECK(f.has("File exists. Replace it?"));
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    const auto request = f.poll();
    REQUIRE(request);
    CHECK(request->path == second);
    CHECK(request->replace_existing);
    const auto fresh = directory.path / "fresh.vscene";
    f.filename(second.string(), fresh.string());
    CHECK_FALSE(f.poll());
    CHECK(f.has("Save"));
    CHECK_FALSE(f.has("Replace"));
    f.click("Save");
    const auto next = f.poll();
    REQUIRE(next);
    CHECK(next->path == fresh);
    CHECK_FALSE(next->replace_existing);
}

TEST_CASE("A save dialog reports filesystem lookup failures without throwing or requesting a write",
          "[editor][ui][save-dialog]") {
    Directory directory;
    Fixture f;
    f.pump();
    f.dialog.open(directory.path / (std::string(300, 'x') + ".vscene"));
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    CHECK_FALSE(f.poll());
    CHECK(f.dialog.visible());
    CHECK_FALSE(f.has("Choose a scene file."));
    CHECK_FALSE(f.has("File exists. Replace it?"));
}

TEST_CASE("Holding Enter cannot confirm a save dialog replacement prompt",
          "[editor][ui][save-dialog]") {
    Directory directory;
    const auto path = directory.existing("existing.vscene");
    Fixture f;
    f.pump();
    f.dialog.open(path);
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    CHECK_FALSE(f.poll());
    REQUIRE(f.has("File exists. Replace it?"));
    for (int i = 0; i < 5; ++i) {
        f.pump({{.kind = EventKind::key_down, .key = Key::enter, .repeat = true}});
        CHECK_FALSE(f.poll());
    }
    CHECK(f.dialog.visible());
    f.pump({{.kind = EventKind::key_up, .key = Key::enter},
            {.kind = EventKind::key_down, .key = Key::enter}});
    const auto confirmed = f.poll();
    REQUIRE(confirmed);
    CHECK(confirmed->replace_existing);
    CHECK(confirmed->path == path);
}

TEST_CASE("Save failures and empty paths keep the dialog open with useful status",
          "[editor][ui][save-dialog]") {
    Directory directory;
    const auto path = directory.path / "fresh.vscene";
    Fixture f;
    f.pump();
    f.dialog.open(path);
    f.pump();
    f.dialog.error("Cannot write scene: permission denied.");
    CHECK(f.dialog.visible());
    CHECK(f.has("Cannot write scene: permission denied."));
    f.filename(path.string(), "", true);
    CHECK_FALSE(f.poll());
    CHECK(f.dialog.visible());
    CHECK(f.has("Enter a scene file path."));
    f.dialog.open(path);
    f.pump();
    CHECK(f.has("Choose a scene file."));
    CHECK_FALSE(f.has("Enter a scene file path."));
    f.pump({{.kind = EventKind::key_down, .key = Key::s, .modifiers = {.control = true}}});
    CHECK_FALSE(f.poll()); // Global save shortcut is the host's modal-routing responsibility.
    CHECK(f.dialog.visible());
}
