#include "../../examples/editor/import_dialog.hpp"
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
text::Font font() {
    auto loaded = text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(loaded);
    return *loaded;
}
struct Directory {
    fs::path path;
    Directory() {
        std::array<char, 40> pattern{};
        std::ranges::copy(std::string_view{"/tmp/vng-import-dialog-XXXXXX"}, pattern.begin());
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
    editor_example::ImportDialog dialog{host};
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
        FAIL("Missing import-dialog caption: " << caption);
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

TEST_CASE("Import dialog browses folders and selects existing vmesh files without loading them",
          "[editor][ui][import-dialog]") {
    Directory directory;
    fs::create_directory(directory.path / "models");
    const auto mesh = directory.file("colored cube.vmesh");
    const auto notes = directory.file("notes.txt");
    const auto size = fs::file_size(mesh);
    Fixture f;
    f.pump();
    CHECK_FALSE(f.dialog.visible());
    CHECK_FALSE(f.poll());
    f.dialog.open(directory.path);
    f.pump();
    REQUIRE(f.dialog.visible());
    CHECK(f.update.capturesKeyboard);
    CHECK(f.has("Import / meshes and effect presets"));
    CHECK(f.has("[Folder] models"));
    CHECK(f.has("colored cube.vmesh"));
    CHECK_FALSE(f.has(notes.filename().string()));
    CHECK(f.find("[Folder] models").position.y < f.find("colored cube.vmesh").position.y);
    f.captions_fit();
    f.click("colored cube.vmesh");
    CHECK_FALSE(f.poll());
    CHECK(f.has(mesh.string()));
    CHECK(f.has("> colored cube.vmesh"));
    f.click("Import");
    const auto request = f.poll();
    REQUIRE(request);
    CHECK(*request == mesh);
    CHECK(f.dialog.visible());
    CHECK(fs::file_size(mesh) == size);
    f.dialog.error("Import failed: the mesh data is invalid.");
    CHECK(f.has("Import failed: the mesh data is invalid."));
    CHECK(f.dialog.visible());
    f.captions_fit();
    f.dialog.close();
    f.pump();
    CHECK_FALSE(f.dialog.visible());
    CHECK_FALSE(f.update.capturesKeyboard);
    CHECK_FALSE(f.has("Import"));
    CHECK_FALSE(f.poll());
}

TEST_CASE("Import dialog lists and accepts effect presets without parsing them", "[editor][ui][import-dialog]") {
    Directory directory;
    const auto preset=directory.file("quiet sun.VEFFECT");
    Fixture f; f.dialog.open(directory.path); f.pump();
    REQUIRE(f.has("quiet sun.VEFFECT"));
    f.click("quiet sun.VEFFECT"); CHECK_FALSE(f.poll());
    f.click("Import"); const auto request=f.poll(); REQUIRE(request); CHECK(*request==preset);
    f.captions_fit();
}

TEST_CASE("Import dialog supports directory navigation Up Refresh and relative filenames",
          "[editor][ui][import-dialog]") {
    Directory directory;
    fs::create_directory(directory.path / "models");
    const auto mesh = directory.file("models/ship.vmesh");
    Fixture f;
    f.dialog.open(directory.path);
    f.pump();
    f.click("[Folder] models");
    CHECK_FALSE(f.poll());
    CHECK(f.has((directory.path / "models").string()));
    CHECK(f.has("ship.vmesh"));
    f.filename((directory.path / "models").string(), "./ship.vmesh", true);
    const auto request = f.poll();
    REQUIRE(request);
    CHECK(*request == mesh);
    f.pump();
    f.click("Up");
    CHECK_FALSE(f.poll());
    CHECK(f.has(directory.path.string()));
    CHECK(f.has("[Folder] models"));
    CHECK_FALSE(f.has("ship.vmesh"));
    const auto added = directory.file("new.vmesh");
    CHECK_FALSE(f.has("new.vmesh"));
    f.click("Refresh");
    CHECK_FALSE(f.poll());
    CHECK(f.has("new.vmesh"));
    f.filename(directory.path.string(), (directory.path / "models").string() + "/", true);
    CHECK_FALSE(f.poll());
    CHECK(f.has("ship.vmesh"));
    CHECK_FALSE(f.has("new.vmesh"));
    f.click("Up");
    CHECK_FALSE(f.poll());
    CHECK(f.has(directory.path.string()));
    CHECK(f.has("new.vmesh"));
    CHECK(fs::exists(added));
}

TEST_CASE("Import dialog Enter requires a fresh key and Escape cancels through text focus",
          "[editor][ui][import-dialog]") {
    Directory directory;
    const auto mesh = directory.file("mesh.vmesh");
    Fixture f;
    f.dialog.open(mesh);
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    CHECK_FALSE(f.dialog.poll()); // Missing raw keyboard consent.
    auto request = f.poll();
    REQUIRE(request);
    CHECK(*request == mesh);
    f.pump({{.kind = EventKind::key_down, .key = Key::enter, .repeat = true}});
    CHECK_FALSE(f.poll());
    f.pump();
    CHECK_FALSE(f.poll());
    f.pump({{.kind = EventKind::key_down, .key = Key::escape}});
    CHECK(f.update.unhandled().empty());
    CHECK_FALSE(f.poll());
    CHECK_FALSE(f.dialog.visible());
    f.dialog.open(mesh);
    f.pump();
    f.click("Cancel");
    CHECK_FALSE(f.poll());
    CHECK_FALSE(f.dialog.visible());
    CHECK(fs::exists(mesh));
}

TEST_CASE("Import dialog rejects missing wrong-format and vanished files without writes",
          "[editor][ui][import-dialog]") {
    Directory directory;
    const auto notes = directory.file("notes.txt");
    const auto mesh = directory.file("mesh.vmesh");
    Fixture f;
    f.dialog.open(directory.path);
    f.pump();
    f.filename(directory.path.string(), "", true);
    CHECK_FALSE(f.poll());
    CHECK(f.has("Enter an existing .vmesh or .veffect file path."));
    // Opening also resets drafts and stale directory entries from earlier use.
    const auto missing = directory.path / "missing/folder/mesh.vmesh";
    f.dialog.open(missing);
    f.pump();
    CHECK_FALSE(f.has("mesh.vmesh"));
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    CHECK_FALSE(f.poll());
    CHECK(f.has("This path does not exist or cannot be accessed."));
    CHECK_FALSE(fs::exists(missing.parent_path()));
    f.filename(missing.string(), notes.string(), true);
    CHECK_FALSE(f.poll());
    CHECK(f.has("Choose an existing regular .vmesh mesh or .veffect preset."));
    f.captions_fit();
    f.dialog.open(directory.path);
    f.pump();
    f.click("mesh.vmesh");
    CHECK_FALSE(f.poll());
    REQUIRE(fs::remove(mesh));
    f.click("Import");
    CHECK_FALSE(f.poll());
    CHECK(f.has("This path does not exist or cannot be accessed."));
    CHECK(fs::exists(notes));
}

TEST_CASE("Import dialog handles long Unicode names empty directories and unreadable entries",
          "[editor][ui][import-dialog]") {
    Directory directory;
    Fixture f;
    f.dialog.open(directory.path);
    f.pump();
    CHECK(f.has("No .vmesh / .veffect files or directories here. Use Up or paste a path."));
    const auto unicode = directory.file("café.VMESH");
    const auto long_file = directory.file(std::string(180, 'W') + ".vmesh");
    fs::create_symlink("missing.vmesh", directory.path / "broken.vmesh");
    directory.file(std::string(1, static_cast<char>(0xFF)) + ".vmesh");
    f.click("Refresh");
    CHECK_FALSE(f.poll());
    CHECK(f.has("café.VMESH"));
    CHECK(f.has(std::string(32, 'W') + "..."));
    CHECK_FALSE(f.has("broken.vmesh"));
    f.captions_fit();
    f.click("café.VMESH");
    CHECK_FALSE(f.poll());
    f.pump({{.kind = EventKind::key_down, .key = Key::enter}});
    auto request = f.poll();
    REQUIRE(request);
    CHECK(*request == unicode);
    f.filename(unicode.string(), long_file.string(), true);
    request = f.poll();
    REQUIRE(request);
    CHECK(*request == long_file);
    f.captions_fit();
}

TEST_CASE("Import dialog bounds enumeration even when a folder contains only other formats",
          "[editor][ui][import-dialog]") {
    Directory directory;
    for (unsigned i = 0; i < 2050; ++i)
        directory.file(std::to_string(i) + ".txt");
    Fixture f;
    f.dialog.open(directory.path);
    f.pump();
    CHECK(f.has("Large directory: showing the first 2048 entries. Paste a full path if missing."));
    f.captions_fit();
    // A complete path remains usable even outside the displayed scan prefix.
    const auto mesh = directory.file("wanted.vmesh");
    f.filename(directory.path.string(), mesh.string(), true);
    const auto request = f.poll();
    REQUIRE(request);
    CHECK(*request == mesh);
}
