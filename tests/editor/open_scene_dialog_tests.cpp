#include "../../examples/editor/open_scene_dialog.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace {
using namespace vng;
using input::EventKind;
using input::Key;
namespace fs = std::filesystem;
constexpr std::string_view title = "Open scene / .vscene projects";
text::Font font() {
    auto loaded = text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(loaded);
    return *loaded;
}
struct Directory {
    fs::path path;
    Directory() {
        std::array<char, 40> pattern{};
        std::ranges::copy(std::string_view{"/tmp/vng-open-scene-dialog-XXXXXX"}, pattern.begin());
        REQUIRE(::mkdtemp(pattern.data()));
        path = pattern.data();
    }
    ~Directory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path file(std::string_view name) const {
        const auto destination = path / name;
        std::ofstream output{destination, std::ios::binary};
        REQUIRE(output);
        output << "Only the host should load these bytes, never the file picker.";
        output.close();
        REQUIRE(output);
        return destination;
    }
};
struct Fixture {
    ui::Screen screen{ui::dark_theme(font())};
    ui::Container host = screen.column().position({100, 100}).width(780).height(560);
    editor_example::OpenSceneDialog dialog{host};
    input::Frame input{.logical_size = {1100, 850}, .framebuffer = {1100, 850}};
    ui::UpdateResult update;
    void pump(std::initializer_list<input::Event> events = {}) {
        input.events = events;
        for (const auto& event : events)
            if (event.kind == EventKind::pointer_down || event.kind == EventKind::pointer_up ||
                event.kind == EventKind::pointer_move)
                input.pointer = event.position;
        auto result = screen.update(input, .016F);
        REQUIRE(result);
        update = std::move(*result);
    }
    std::optional<fs::path> poll() { return dialog.poll(input.events); }
    ui::DrawList draw() {
        auto result = screen.draw_list();
        REQUIRE(result);
        return std::move(*result);
    }
    ui::TextDraw find(std::string_view caption) {
        for (const auto& command : draw().commands)
            if (const auto* text = std::get_if<ui::TextDraw>(&command);
                text && text->text == caption)
                return *text;
        FAIL("Missing open-scene caption: " << caption);
        return {};
    }
    bool has(std::string_view caption) {
        return std::ranges::any_of(draw().commands, [&](const auto& command) {
            const auto* text = std::get_if<ui::TextDraw>(&command);
            return text && text->text == caption;
        });
    }
    void click(std::string_view caption) {
        const auto text = find(caption);
        const Vec2 at{std::max(text.position.x + 3, text.clip.x + 3), text.position.y + 8};
        pump({{.kind = EventKind::pointer_down, .position = at},
              {.kind = EventKind::pointer_up, .position = at}});
    }
    void filename(std::string_view current, std::string next, bool submit = false) {
        click(current);
        const input::Event select{
            .kind = EventKind::key_down, .key = Key::a, .modifiers = {.control = true}};
        const input::Event text{.kind = EventKind::text, .text = std::move(next)};
        if (submit)
            pump({select, text, {.kind = EventKind::key_down, .key = Key::enter}});
        else
            pump({select, text});
    }
    void captions_fit() {
        for (const auto& command : draw().commands)
            if (const auto* text = std::get_if<ui::TextDraw>(&command)) {
                if (text->text.starts_with('/'))
                    continue; // Full paths are intentionally scrollable text, not captions.
                INFO(text->text);
                const auto measure = text->font.measure(text->text, static_cast<u32>(text->size));
                REQUIRE(measure);
                CHECK(text->position.x >= text->clip.x);
                CHECK(text->position.x + measure->width <= text->clip.x + text->clip.width + .01F);
                CHECK(text->position.y >= text->clip.y);
                CHECK(text->position.y + text->size <= text->clip.y + text->clip.height);
            }
    }
};
} // namespace

TEST_CASE("Scene file detection matches the vscene extension regardless of case",
          "[editor][ui][open-scene-dialog]") {
    using editor_example::is_scene_file;
    CHECK(is_scene_file("shot.vscene"));
    CHECK(is_scene_file("/tmp/SHOT.VSCENE"));
    CHECK(is_scene_file("fleet.reveal.vScene"));
    CHECK_FALSE(is_scene_file("ship.vmesh"));
    CHECK_FALSE(is_scene_file("preset.veffect"));
    CHECK_FALSE(is_scene_file("vscene"));
    CHECK_FALSE(is_scene_file("notes.vscene.txt"));
}

TEST_CASE("Open scene dialog lists only scenes beside the current one and preselects it",
          "[editor][ui][open-scene-dialog]") {
    Directory directory;
    fs::create_directory(directory.path / "shots");
    const auto current = directory.file("edited.vscene");
    const auto other = directory.file("FLEET.VSCENE");
    const auto mesh = directory.file("cube.vmesh");
    const auto preset = directory.file("quiet sun.veffect");
    const auto notes = directory.file("notes.txt");
    const auto size = fs::file_size(current);
    Fixture f;
    f.pump();
    CHECK_FALSE(f.dialog.visible());
    CHECK_FALSE(f.poll());
    f.dialog.open(current);
    f.pump();
    REQUIRE(f.dialog.visible());
    CHECK(f.update.capturesKeyboard);
    CHECK(f.has(title));
    CHECK(f.has("[Folder] shots"));
    CHECK(f.has("> edited.vscene"));
    CHECK(f.has("FLEET.VSCENE"));
    CHECK(f.has(current.string()));
    for (const auto& excluded : {mesh, preset, notes})
        CHECK_FALSE(f.has(excluded.filename().string()));
    CHECK(f.find("[Folder] shots").position.y < f.find("> edited.vscene").position.y);
    CHECK(f.has("Open"));
    CHECK_FALSE(f.has("Import"));
    f.captions_fit();
    f.click("FLEET.VSCENE");
    CHECK_FALSE(f.poll());
    CHECK(f.has("> FLEET.VSCENE"));
    CHECK(f.has("edited.vscene"));
    CHECK(f.has("Scene selected. Click Open or press Enter."));
    f.click("Open");
    const auto request = f.poll();
    REQUIRE(request);
    CHECK(*request == other);
    CHECK(f.dialog.visible()); // The host loads, then closes on success.
    CHECK(fs::file_size(current) == size);
    CHECK(fs::file_size(other) == size);
    f.dialog.error("Cannot load scene: unknown version.");
    CHECK(f.has("Cannot load scene: unknown version."));
    CHECK(f.dialog.visible());
    f.dialog.close();
    f.pump();
    CHECK_FALSE(f.dialog.visible());
    CHECK_FALSE(f.update.capturesKeyboard);
    CHECK_FALSE(f.has("Open"));
    CHECK_FALSE(f.poll());
}

TEST_CASE("Open scene dialog refuses other formats and missing paths but opens folders",
          "[editor][ui][open-scene-dialog]") {
    Directory directory;
    fs::create_directory(directory.path / "shots");
    const auto nested = directory.file("shots/nested.vscene");
    const auto mesh = directory.file("cube.vmesh");
    Fixture f;
    f.dialog.open(directory.path);
    f.pump();
    // A folder is listed, so the selection prompt shows instead of the empty notice.
    CHECK_FALSE(f.has("No .vscene files or directories here. Use Up or paste a path."));
    CHECK(f.has("Select a scene, then Open or Enter. A folder click opens it."));
    f.filename(directory.path.string(), mesh.string(), true);
    CHECK_FALSE(f.poll());
    CHECK(f.has("Choose an existing regular .vscene scene."));
    f.filename(mesh.string(), (directory.path / "missing.vscene").string(), true);
    CHECK_FALSE(f.poll());
    CHECK(f.has("This path does not exist or cannot be accessed."));
    f.filename((directory.path / "missing.vscene").string(), "", true);
    CHECK_FALSE(f.poll());
    CHECK(f.has("Enter an existing .vscene file path."));
    f.dialog.open(directory.path);
    f.pump();
    f.filename(directory.path.string(), "shots", true); // Relative to the listed folder.
    CHECK_FALSE(f.poll());
    CHECK(f.has("nested.vscene"));
    f.click("nested.vscene");
    CHECK_FALSE(f.poll());
    f.click("Open");
    const auto request = f.poll();
    REQUIRE(request);
    CHECK(*request == nested);
    f.pump({{.kind = EventKind::key_down, .key = Key::escape}});
    CHECK_FALSE(f.poll());
    CHECK_FALSE(f.dialog.visible());
}

TEST_CASE("Open scene dialog reports an empty folder and unopenable start paths",
          "[editor][ui][open-scene-dialog]") {
    Directory directory;
    Fixture f;
    f.dialog.open(directory.path);
    f.pump();
    CHECK(f.has("No .vscene files or directories here. Use Up or paste a path."));
    f.captions_fit();
    f.dialog.open(fs::path{std::string("bad\x01name.vscene")});
    f.pump();
    CHECK(f.has("Cannot open this path. Paste a directory or .vscene path."));
    f.captions_fit();
}
