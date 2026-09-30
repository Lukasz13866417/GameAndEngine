#include "../../examples/review/review_file.hpp"
#include "../../examples/editor/selection.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <sys/stat.h>

namespace {
using namespace vng;
namespace fs = std::filesystem;

// A fresh directory, removed with everything in it afterwards.
struct Scratch {
    fs::path path;
    Scratch() {
        auto pattern = (fs::temp_directory_path() / "vng-review-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()));
        path = pattern;
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

review::Note note(u32 id, std::string candidate, std::string text) {
    review::Note n;
    n.id = id;
    n.candidate = std::move(candidate);
    n.time = 31;
    n.view = {0, 1};
    n.text = std::move(text);
    n.created = "2026-09-28T18:41:00Z";
    return n;
}
// A comparison with the text a reviewer really writes: quotes, backslashes,
// tabs, new lines and UTF-8, and one scene outside the review's directory.
review::Review sample(const fs::path& dir) {
    review::Review r;
    r.title = "Cage \"replacement\": three designs";
    r.created = "2026-09-28T18:27:30Z";
    r.range = std::array{30.F, 47.5F};
    r.summary = "Overall: C.\nB's tanks read as \\clutter\\.\tTabbed.";
    r.pick = "C";
    r.candidates = {
        {"A", "Relay dish", "A 6 km dish.", "", dir / "scenes" / "a.vscene", 0},
        {"C", "Mass-driver catcher", "Funnel, \"bay\" and racks.\nSecond line.", "Best — the lights sell it. ✓",
         dir.parent_path() / "elsewhere" / "c.vscene", 1.25F},
    };
    auto pinned = note(1, "C", "Too bright\nhere");
    pinned.time = 40.125F;
    pinned.object = 1002;
    pinned.object_name = "GATEWAY / Arabian orbital yard";
    pinned.point = Vec3{.0062F, 874.34F, -1094.66F};
    pinned.view = {.372F, .72F};
    pinned.status = "resolved";
    pinned.reply = "Dimmed the rim lights to 60 %.";
    r.notes = {pinned, note(4, "A", "Sky is empty")};
    return r;
}
const review::Note* find_note(const review::Review& r, u32 id) {
    const auto found = std::ranges::find(r.notes, id, &review::Note::id);
    return found == r.notes.end() ? nullptr : &*found;
}
std::string refusal(const review::Review& r) {
    const auto valid = review::validate(r);
    return valid ? std::string{} : valid.error().message;
}
} // namespace

TEST_CASE("A review reads back exactly as it was saved", "[review][file]") {
    Scratch dir;
    const auto file = dir.path / "cage.vreview";
    const auto r = sample(dir.path);
    REQUIRE(review::save_review(file, r));
    const auto read = review::read_review(file);
    REQUIRE(read);
    CHECK(*read == r);
    // Saving what was read writes the same bytes.
    const auto again = review::encode_review(*read, file), first = review::encode_review(r, file);
    REQUIRE(again);
    REQUIRE(first);
    CHECK(*again == *first);
}

TEST_CASE("Review text stays one readable line per field", "[review][file]") {
    Scratch dir;
    const auto file = dir.path / "cage.vreview";
    const auto text = review::encode_review(sample(dir.path), file);
    REQUIRE(text);
    CHECK(text->starts_with("vreview 1.0\nreview = 1;\n"));
    CHECK(text->contains(R"(text = "Too bright\nhere";)"));
    CHECK(text->contains(R"(title = "Cage \"replacement\": three designs";)"));
    CHECK(text->contains(R"(summary = "Overall: C.\nB's tanks read as \\clutter\\.\tTabbed.";)"));
    CHECK(text->contains("thoughts = \"Best — the lights sell it. ✓\";"));
}

TEST_CASE("Scene paths are stored relative to the review file", "[review][file]") {
    Scratch dir;
    const auto file = dir.path / "cage.vreview";
    const auto text = review::encode_review(sample(dir.path), file);
    REQUIRE(text);
    CHECK(text->contains(R"(scene = "scenes/a.vscene";)"));
    CHECK(text->contains(R"(scene = "../elsewhere/c.vscene";)"));

    // A hand-written review needs only titles, ids, labels and scenes.
    const auto parsed = review::parse_review(R"(vreview 1.0
review = 1;
title = "Two takes";
candidates = [
    { id = "A"; label = "Absolute"; scene = "/scenes/a.vscene"; },
    { id = "B"; label = "Relative"; scene = "takes/../b.vscene"; offset = -2.5; },
];
)", file);
    REQUIRE(parsed);
    CHECK(parsed->candidates[0].scene == fs::path("/scenes/a.vscene"));
    CHECK(parsed->candidates[1].scene == dir.path / "b.vscene");
    CHECK(parsed->candidates[1].offset == -2.5F);
    CHECK(parsed->comparison());
    CHECK_FALSE(parsed->range);
    CHECK(parsed->notes.empty());
    CHECK(parsed->next_note_id() == 1);
}

TEST_CASE("Reviews that break the format's rules are refused with a reason", "[review][file]") {
    Scratch dir;
    const auto good = sample(dir.path);
    REQUIRE(refusal(good).empty());
    const auto changed = [&](auto change) {
        auto r = good;
        change(r);
        return refusal(r);
    };
    CHECK(changed([](auto& r) { r.title.clear(); }).contains("title"));
    CHECK(changed([](auto& r) { r.title = "two\nlines"; }).contains("title"));
    CHECK(changed([](auto& r) { r.candidates.clear(); }).contains("1 to 4 candidates"));
    CHECK(changed([](auto& r) {
        for (const auto* id : {"B", "D", "E"}) r.candidates.push_back({id, "More", "", "", "x.vscene", 0});
    }).contains("1 to 4 candidates"));
    CHECK(changed([](auto& r) { r.candidates[1].id = "A"; }).contains("Duplicate candidate id"));
    CHECK(changed([](auto& r) { r.candidates[0].id = "A 1"; }).contains("without spaces"));
    CHECK(changed([](auto& r) { r.candidates[0].label.clear(); }).contains("label"));
    CHECK(changed([](auto& r) { r.candidates[0].about = "bell\a"; }).contains("control characters"));
    CHECK(changed([](auto& r) { r.candidates[0].thoughts = std::string(review::max_text_bytes + 1, 'x'); }).contains("too long"));
    CHECK(changed([](auto& r) { r.candidates[0].scene.clear(); }).contains("names no scene"));
    CHECK(changed([](auto& r) { r.candidates[0].offset = std::numeric_limits<f32>::quiet_NaN(); }).contains("non-finite offset"));
    CHECK(changed([](auto& r) { r.range = std::array{47.F, 30.F}; }).contains("range"));
    CHECK(changed([](auto& r) { r.range = std::array{-1.F, 30.F}; }).contains("range"));
    CHECK(changed([](auto& r) { r.pick = "B"; }).contains("pick"));
    CHECK(changed([](auto& r) { r.verdict = "fine"; }).contains("verdict"));
    CHECK(changed([](auto& r) { r.notes[0].id = 0; }).contains("unique and positive"));
    CHECK(changed([](auto& r) { r.notes[1].id = 1; }).contains("unique and positive"));
    CHECK(changed([](auto& r) { r.notes[0].candidate = "B"; }).contains("names no candidate"));
    CHECK(changed([](auto& r) { r.notes[0].time = std::numeric_limits<f32>::infinity(); }).contains("non-finite time"));
    CHECK(changed([](auto& r) { r.notes[0].view.x = 1.5F; }).contains("normalized"));
    CHECK(changed([](auto& r) { r.notes[0].status = "done"; }).contains("status"));
    CHECK(changed([](auto& r) { r.notes[0].reply = "carriage\rreturn"; }).contains("control characters"));
    CHECK(changed([](auto& r) { r.notes.resize(review::max_notes + 1); }).contains("Too many notes"));

    const auto file = dir.path / "r.vreview";
    CHECK_FALSE(review::parse_review("vscene 1.0\nreview = 1;\n", file));
    CHECK_FALSE(review::parse_review("vreview 1.0\nreview = 2;\ntitle = \"T\";\ncandidates = [];\n", file));
    // A raw new line inside a string is not the format; \n is.
    CHECK_FALSE(review::parse_review("vreview 1.0\nreview = 1;\ntitle = \"T\";\nsummary = \"a\nb\";\n"
                                     "candidates = [ { id = \"A\"; label = \"L\"; scene = \"s.vscene\"; }, ];\n", file));
    CHECK_FALSE(review::read_review(dir.path / "missing.vreview"));
    // Nothing that fails the rules is ever written.
    auto bad = good;
    bad.verdict = "fine";
    CHECK_FALSE(review::save_review(file, bad));
    CHECK_FALSE(fs::exists(file));
}

TEST_CASE("Review paths containing parent components retain their scene references", "[review][file]") {
    Scratch dir;
    const auto original = sample(dir.path);
    for (const auto& file : {dir.path / "unused" / ".." / "cage.vreview",
                             fs::relative(dir.path) / "unused" / ".." / "cage.vreview"}) {
        const auto encoded = review::encode_review(original, file);
        REQUIRE(encoded);
        const auto parsed = review::parse_review(*encoded, file);
        REQUIRE(parsed);
        CHECK(*parsed == original);
    }
}

TEST_CASE("Saving replaces the file in one step and keeps its permissions", "[review][file]") {
    Scratch dir;
    const auto file = dir.path / "r.vreview";
    auto r = sample(dir.path);
    const auto previous = ::umask(022);
    const auto first = review::save_review(file, r);
    ::umask(previous);
    REQUIRE(first);
    CHECK(review::stamp_of(file) == *first);
    using fs::perms;
    CHECK(fs::status(file).permissions() == (perms::owner_read | perms::owner_write | perms::group_read | perms::others_read));

    fs::permissions(file, perms::owner_read | perms::owner_write | perms::group_read);
    r.summary = "Changed";
    const auto second = review::save_review(file, r);
    REQUIRE(second);
    CHECK(*second != *first);
    CHECK(review::stamp_of(file) == *second);
    CHECK(fs::status(file).permissions() == (perms::owner_read | perms::owner_write | perms::group_read));
    CHECK(review::read_review(file)->summary == "Changed");
    // No temporary file is left beside it.
    CHECK(std::distance(fs::directory_iterator(dir.path), fs::directory_iterator{}) == 1);
    CHECK_FALSE(review::stamp_of(dir.path / "missing.vreview"));
}

TEST_CASE("Saving through a symbolic link replaces the file it names", "[review][file]") {
    Scratch dir;
    const auto real = dir.path / "real.vreview", link = dir.path / "link.vreview";
    auto r = sample(dir.path);
    REQUIRE(review::save_review(real, r));
    fs::create_symlink(real, link);
    r.summary = "Through the link";
    REQUIRE(review::save_review(link, r));
    CHECK(fs::is_symlink(link));
    CHECK(review::read_review(real)->summary == "Through the link");
}

TEST_CASE("Recovering an unreadable review preserves it before replacing anything", "[review][file]") {
    Scratch dir;
    const auto file = dir.path / "r.vreview", backup = dir.path / "r.vreview.unreadable";
    const auto read_bytes = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    };
    const std::string interrupted = "vreview 1.0\ntitle = \"a writer was interrupted";
    { std::ofstream out(file); out << interrupted; }
    auto r = sample(dir.path);
    SECTION("a successful recovery keeps the exact unreadable bytes") {
        REQUIRE(review::save_review(file, r, backup));
        CHECK(read_bytes(backup) == interrupted);
        CHECK(review::read_review(file) == r);
    }
    SECTION("a previous backup is never overwritten") {
        { std::ofstream out(backup); out << "older recovery"; }
        CHECK_FALSE(review::save_review(file, r, backup));
        CHECK(read_bytes(file) == interrupted);
        CHECK(read_bytes(backup) == "older recovery");
    }
    SECTION("a failed backup leaves the original file alone") {
        fs::create_directory(backup);
        CHECK_FALSE(review::save_review(file, r, backup));
        CHECK(read_bytes(file) == interrupted);
        CHECK(fs::is_directory(backup));
    }
}

TEST_CASE("Merging keeps both sides' edits, and mine where both edited one field", "[review][merge]") {
    Scratch dir;
    const auto base = sample(dir.path);
    auto mine = base, theirs = base;
    // Here, the reviewer rewrites note 4, thinks about A and adds note 5.
    mine.notes[1].text = "Sky is empty; add stars";
    mine.candidates[0].thoughts = "Too plain.";
    mine.notes.push_back(note(5, "A", "Mine"));
    mine.summary = "Mine";
    // There, an agent answers note 4, edits a description and adds its own note 5.
    theirs.notes[1].reply = "Added a star field.";
    theirs.notes[1].status = "resolved";
    theirs.candidates[1].about = "Updated description";
    theirs.notes.push_back(note(5, "C", "Theirs"));
    theirs.summary = "Theirs";

    const auto merged = review::merge(base, mine, theirs);
    REQUIRE(refusal(merged).empty());
    CHECK(merged.summary == "Mine");
    CHECK(merged.candidates[0].thoughts == "Too plain.");
    CHECK(merged.candidates[1].about == "Updated description");
    const auto* answered = find_note(merged, 4);
    REQUIRE(answered);
    CHECK(answered->text == "Sky is empty; add stars");
    CHECK(answered->reply == "Added a star field.");
    CHECK(answered->status == "resolved");
    // Both added a note 5: theirs keeps the id and mine takes the next one.
    REQUIRE(find_note(merged, 5));
    REQUIRE(find_note(merged, 6));
    CHECK(find_note(merged, 5)->text == "Theirs");
    CHECK(find_note(merged, 6)->text == "Mine");
    CHECK(merged.notes.size() == 4);
}

TEST_CASE("Merging keeps a deletion unless the other side changed what was deleted", "[review][merge]") {
    Scratch dir;
    auto base = sample(dir.path);
    base.notes.push_back(note(7, "A", "Seven"));
    base.notes.push_back(note(8, "A", "Eight"));
    auto mine = base, theirs = base;
    std::erase_if(mine.notes, [](const auto& n) { return n.id == 1 || n.id == 4; });
    theirs.notes[1].reply = "Answered after you deleted it";
    std::erase_if(theirs.notes, [](const auto& n) { return n.id == 7 || n.id == 8; });
    mine.notes.back().text = "Eight, edited after they deleted it";

    const auto merged = review::merge(base, mine, theirs);
    CHECK_FALSE(find_note(merged, 1)); // deleted here, untouched there
    REQUIRE(find_note(merged, 4));     // deleted here, answered there
    CHECK(find_note(merged, 4)->reply == "Answered after you deleted it");
    CHECK_FALSE(find_note(merged, 7)); // deleted there, untouched here
    REQUIRE(find_note(merged, 8));     // deleted there, edited here
    CHECK(find_note(merged, 8)->text == "Eight, edited after they deleted it");
}

TEST_CASE("Merging with one side unchanged gives the other side", "[review][merge]") {
    Scratch dir;
    const auto base = sample(dir.path);
    auto edited = base;
    edited.notes[0].text = "Edited";
    edited.notes.push_back(note(9, "A", "Added"));
    std::erase_if(edited.notes, [](const auto& n) { return n.id == 4; });
    edited.verdict = "needs work";
    CHECK(review::merge(base, edited, base) == edited);
    CHECK(review::merge(base, base, edited) == edited);
    CHECK(review::merge(base, edited, edited) == edited);
}

TEST_CASE("Merging drops notes on a candidate that is gone, and a pick of it", "[review][merge]") {
    Scratch dir;
    const auto base = sample(dir.path);
    auto mine = base, theirs = base;
    mine.notes.push_back(note(5, "C", "On C"));
    std::erase_if(theirs.candidates, [](const auto& c) { return c.id == "C"; });
    theirs.candidates.push_back({"D", "New take", "", "", dir.path / "d.vscene", 0});
    theirs.pick.clear();
    std::erase_if(theirs.notes, [](const auto& n) { return n.candidate == "C"; });

    const auto merged = review::merge(base, mine, theirs);
    REQUIRE(refusal(merged).empty());
    REQUIRE(merged.candidates.size() == 2);
    CHECK(merged.candidates[0].id == "A");
    CHECK(merged.candidates[1].id == "D");
    CHECK(merged.pick.empty());
    REQUIRE(merged.notes.size() == 1);
    CHECK(merged.notes[0].id == 4);
}

TEST_CASE("Review times are UTC to the second", "[review][file]") {
    CHECK(std::regex_match(review::timestamp(), std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)")));
}

namespace {
using namespace editor_example;
constexpr Extent2D extent{800, 600};
// One triangle at the origin, facing the editor camera 8 units away.
State triangle() {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position", {content::vmesh::ScalarType::Float32, 3}, std::vector<f32>{-1, -1, 0, 1, -1, 0, 0, 1, 0}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    State state{.document = {.mesh = std::move(*mesh)}};
    state.viewport.editor_camera.yaw = state.viewport.editor_camera.pitch = 0;
    state.viewport.editor_camera.distance = 8;
    instance_transform(state, 1)->position = {};
    instance_transform(state, 1)->scale = 1;
    sun_settings(state, 2)->visible = false;
    return state;
}
Vec2 projected(const State& state, Vec3 p) {
    const auto snapshot = camera(state).snapshot(extent);
    REQUIRE(snapshot);
    const auto& m = snapshot->view_projection;
    const auto x = m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0];
    const auto y = m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1];
    const auto w = m[0][3] * p.x + m[1][3] * p.y + m[2][3] * p.z + m[3][3];
    return {(x / w + 1) * .5F, (1 - y / w) * .5F};
}
} // namespace

TEST_CASE("Picking finds where the view ray meets the entity", "[review][pick]") {
    const auto state = triangle();
    const auto view = camera(state);
    for (const auto target : {Vec3{}, Vec3{.25F, -.5F, 0}, Vec3{-.6F, -.9F, 0}, Vec3{0, .8F, 0}}) {
        const auto pixel = projected(state, target);
        const auto hit = pick(state, pixel, extent);
        REQUIRE(hit);
        CHECK(hit->object == 1);
        CHECK(hit->point.x == Catch::Approx(target.x).margin(1e-3));
        CHECK(hit->point.y == Catch::Approx(target.y).margin(1e-3));
        CHECK(hit->point.z == Catch::Approx(target.z).margin(1e-3));
        // The object is what pick_object reports; a presented camera gives the same hit.
        CHECK(pick_object(state, pixel, extent) == hit->object);
        const auto presented = pick(state, pixel, extent, &view);
        REQUIRE(presented);
        CHECK(presented->point == hit->point);
    }
    CHECK_FALSE(pick(state, projected(state, {.9F, .9F, 0}), extent));
    CHECK_FALSE(pick(state, {.02F, .02F}, extent));
}

TEST_CASE("Invisible editor camera glyphs do not intercept review pins", "[review][pick]") {
    auto state = triangle();
    const auto id = instantiate(state, BlueprintId::camera);
    REQUIRE(id);
    instance_transform(state, *id)->position = {0, 0, 4};
    const auto editor_hit = pick(state, {.5F, .5F}, extent);
    REQUIRE(editor_hit);
    CHECK(editor_hit->object == *id);
    const auto scene_hit = pick(state, {.5F, .5F}, extent, nullptr, nullptr, {.camera_glyphs = false});
    REQUIRE(scene_hit);
    CHECK(scene_hit->object == 1);
    CHECK(scene_hit->point.z == Catch::Approx(0).margin(1e-3));
}
