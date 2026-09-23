#include "../../examples/editor/scene_file.hpp"
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/animation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstring>
#include <fstream>
#include <limits>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace vng;
using namespace editor_example;

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::array<char, 64> name{};
        std::strcpy(name.data(), "/tmp/vng-scene-file-test-XXXXXX");
        const auto created = ::mkdtemp(name.data());
        REQUIRE(created);
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

struct CurrentDirectory {
    fs::path original{fs::current_path()};
    explicit CurrentDirectory(const fs::path& path) { fs::current_path(path); }
    ~CurrentDirectory() {
        std::error_code ignored;
        fs::current_path(original, ignored);
    }
};

struct WriteLimit {
    struct rlimit original {};
    struct sigaction action {};
    WriteLimit() {
        REQUIRE(::getrlimit(RLIMIT_FSIZE, &original) == 0);
        struct sigaction ignored {};
        ignored.sa_handler = SIG_IGN;
        ::sigemptyset(&ignored.sa_mask);
        REQUIRE(::sigaction(SIGXFSZ, &ignored, &action) == 0);
        auto limited = original;
        limited.rlim_cur = 128;
        if (::setrlimit(RLIMIT_FSIZE, &limited) != 0) {
            ::sigaction(SIGXFSZ, &action, nullptr);
            FAIL("Cannot install deterministic scene-write size limit");
        }
    }
    ~WriteLimit() {
        ::setrlimit(RLIMIT_FSIZE, &original);
        ::sigaction(SIGXFSZ, &action, nullptr);
    }
};

State scene() {
    content::vmesh::Document document;
    document.vertex_count = 3;
    document.vertex_fields = {{"position",
                               {content::vmesh::ScalarType::Float32, 3},
                               std::vector<f32>{0, 0, 0, 1, 0, 0, 0, 1, 0}}};
    document.faces = {{0, 1, 2}};
    auto mesh = editor::EditableMesh::create(std::move(document));
    REQUIRE(mesh);
    return {.document = {.mesh = std::move(*mesh)}};
}
std::string bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
void write(const fs::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file << text;
    file.close();
    REQUIRE(file);
}
std::string encoded(const State& state) {
    auto result = encode_scene(state);
    REQUIRE(result);
    return std::move(*result);
}
void no_temporaries(const fs::path& path) {
    for (const auto& entry : fs::recursive_directory_iterator(path))
        CHECK_FALSE(entry.path().filename().native().starts_with(".vng-scene-"));
}
} // namespace

TEST_CASE("Saving bakes placement copies while retaining unapplied blueprint separation", "[editor][file][whole-mesh]") {
    TemporaryDirectory temporary;SceneFile file;auto state=scene();
    auto applied=Mat4::identity();applied[0][0]=2;
    auto draft=applied;draft[1][1]=3;
    state.document.mesh_placements[BlueprintId::mesh]={applied,draft};
    const auto original=state.document.mesh.document();
    const auto path=temporary.path/"placement.vscene";
    REQUIRE(file.save_as(path,state));
    auto loaded=decode(bytes(path));REQUIRE(loaded);
    CHECK(loaded->document.mesh_placements.empty());
    CHECK(loaded->document.mesh.position(1)==Vec3{2,0,0});
    CHECK(loaded->document.mesh.position(2)==Vec3{0,1,0});
    CHECK(mesh_edit_geometry(*loaded,BlueprintId::mesh)->position(2)==Vec3{0,3,0});
    CHECK(state.document.mesh.document()==original);CHECK(state.document.mesh_drafts.empty());
    REQUIRE(apply_mesh_draft(state,BlueprintId::mesh));REQUIRE(file.save(state));
    loaded=decode(bytes(path));REQUIRE(loaded);
    CHECK_FALSE(has_mesh_draft(*loaded,BlueprintId::mesh));
    CHECK(loaded->document.mesh.position(2)==Vec3{0,3,0});
}
TEST_CASE("Scene Save targets the current file and preserves permissions across repeated writes",
          "[editor][file]") {
    TemporaryDirectory temporary;
    SceneFile file;
    auto state = scene();
    CHECK_FALSE(file.path());
    CHECK_FALSE(file.save(state));
    const auto path = temporary.path / "current.vscene";
    REQUIRE(file.save_as(path, state));
    SECTION("Saving a draft does not publish it or discard it on reload") {
        const auto original=state.document.mesh.document();
        REQUIRE(begin_mesh_draft(state,BlueprintId::mesh));
        REQUIRE(mesh_edit_geometry(state,BlueprintId::mesh)->set_position(0,{2,3,4}));
        ++state.document.revision;
        REQUIRE(file.save(state));
        auto loaded=file.load(path);REQUIRE(loaded);
        CHECK(mesh_geometry(*loaded,BlueprintId::mesh)->document()==original);
        CHECK(mesh_edit_geometry(*loaded,BlueprintId::mesh)->position(0)==Vec3{2,3,4});
        REQUIRE(apply_mesh_draft(*loaded,BlueprintId::mesh));
        CHECK(mesh_geometry(*loaded,BlueprintId::mesh)->position(0)==Vec3{2,3,4});
        auto still_on_disk=file.load(path);REQUIRE(still_on_disk);
        CHECK(mesh_geometry(*still_on_disk,BlueprintId::mesh)->document()==original);
        return;
    }
    REQUIRE(file.path());
    CHECK(*file.path() == path);
    const auto permissions = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
    fs::permissions(path, permissions);
    const auto unrelated = temporary.path / "textbox-value.vscene";
    write(unrelated, "unrelated UI filename must not control Save");
    for (int i = 0; i < 5; ++i) {
        (*editor_example::sun_settings(state, 2)).bloom = static_cast<f32>(i) * 0.1F;
        ++state.document.revision;
        REQUIRE(file.save(state));
        CHECK(bytes(path) == encoded(state));
        CHECK((fs::status(path).permissions() & fs::perms::all) == permissions);
        CHECK(*file.path() == path);
    }
    CHECK(bytes(unrelated) == "unrelated UI filename must not control Save");
    no_temporaries(temporary.path);
}

TEST_CASE("Save As changes the current file only after success and requires explicit replacement",
          "[editor][file]") {
    TemporaryDirectory temporary;
    SceneFile file;
    auto state = scene();
    const auto first = temporary.path / "first.vscene";
    const auto second = temporary.path / "second.vscene";
    REQUIRE(file.save_as(first, state));
    const auto original = bytes(first);
    write(second, "existing destination");
    CHECK_FALSE(file.save_as(second, state));
    CHECK(file.path() == std::optional{first});
    CHECK(bytes(second) == "existing destination");
    (*editor_example::sun_settings(state, 2)).white_spots = true;
    ++state.document.revision;
    REQUIRE(file.save_as(second, state, true));
    CHECK(file.path() == std::optional{second});
    CHECK(bytes(second) == encoded(state));
    CHECK(bytes(first) == original);
    (*editor_example::sun_settings(state, 2)).radius = 2;
    ++state.document.revision;
    REQUIRE(file.save(state));
    CHECK(bytes(first) == original);
    CHECK(bytes(second) == encoded(state));
    const auto third = temporary.path / "third.vscene";
    REQUIRE(file.save_as(third, state));
    CHECK(file.path() == std::optional{third});
    no_temporaries(temporary.path);
}

TEST_CASE("Successful loading establishes the filename and failed loading preserves it",
          "[editor][file]") {
    TemporaryDirectory temporary;
    SceneFile seed, file;
    auto state = scene();
    const auto valid = temporary.path / "valid.vscene";
    REQUIRE(seed.save_as(valid, state));
    auto loaded = file.load(valid);
    REQUIRE(loaded);
    CHECK(encoded(*loaded) == encoded(state));
    CHECK(file.path() == std::optional{valid});
    const auto malformed = temporary.path / "malformed.vscene";
    write(malformed, "not a scene");
    auto failure = file.load(malformed);
    REQUIRE_FALSE(failure);
    CHECK(failure.error().path == malformed);
    CHECK(file.path() == std::optional{valid});
    CHECK_FALSE(file.load(temporary.path / "absent.vscene"));
    CHECK_FALSE(file.load(temporary.path));
    CHECK_FALSE(file.load({}));
    CHECK(file.path() == std::optional{valid});
    const auto oversized = temporary.path / "oversized.vscene";
    write(oversized, std::string(32 * 1024 * 1024, 'x'));
    CHECK_FALSE(file.load(oversized));
    CHECK(file.path() == std::optional{valid});
    no_temporaries(temporary.path);
}

TEST_CASE("Scene files persist scene cameras while editor navigation remains private",
          "[editor][file][camera]") {
    TemporaryDirectory temporary;
    SceneFile file;
    auto state = scene();
    const auto camera = ensure_camera(state, {65, -23, 7, {2, -1, 3}});
    REQUIRE(camera);
    state.viewport.editor_camera = {-20, 5, 3, {4, 2, 1}};
    const auto path = temporary.path / "camera.vscene";
    REQUIRE(file.save_as(path, state));
    auto loaded = file.load(path);
    REQUIRE(loaded);
    CHECK(*find_instance(*loaded, *camera) == *find_instance(state, *camera));
    CHECK(loaded->viewport.editor_camera == *evaluate_camera(*loaded, 0));
    const auto before_navigation = bytes(path);
    state.viewport.editor_camera = {-75, -30, 15, {1, 5, 2}};
    REQUIRE(file.save(state));
    CHECK(bytes(path) == before_navigation);
    find_instance(state, *camera)->transform.rotation.y = 80;
    REQUIRE(file.save(state));
    loaded = file.load(path);
    REQUIRE(loaded);
    CHECK(find_instance(*loaded, *camera)->transform.rotation.y == 80);
    CHECK(evaluate_camera(*loaded, 0)->yaw == 80);
}
TEST_CASE("Failed encoding and failed temporary writes leave the original scene intact",
          "[editor][file]") {
    TemporaryDirectory temporary;
    SceneFile file;
    auto state = scene();
    const auto path = temporary.path / "preserved.vscene";
    REQUIRE(file.save_as(path, state));
    const auto original = bytes(path);
    auto bad = state;
    (*editor_example::sun_settings(bad, 2)).radius = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(file.save(bad));
    CHECK_FALSE(file.save_as(temporary.path / "invalid.vscene", bad));
    CHECK_FALSE(fs::exists(temporary.path / "invalid.vscene"));
    CHECK(bytes(path) == original);
    (*editor_example::sun_settings(state, 2)).radius = 2;
    ++state.document.revision;
    content::Result<void> failed_write;
    {
        WriteLimit limited;
        failed_write = file.save(state);
    }
    REQUIRE_FALSE(failed_write);
    CHECK(file.path() == std::optional{path});
    CHECK(bytes(path) == original);
    no_temporaries(temporary.path);
    {
        WriteLimit limited;
        failed_write = file.save_as(temporary.path / "partial.vscene", state);
    }
    REQUIRE_FALSE(failed_write);
    CHECK_FALSE(fs::exists(temporary.path / "partial.vscene"));
    CHECK(file.path() == std::optional{path});
    no_temporaries(temporary.path);
    REQUIRE(file.save(state));
    CHECK(bytes(path) == encoded(state));
}

TEST_CASE("Scene paths reject symlinks directories special files and embedded null bytes",
          "[editor][file]") {
    TemporaryDirectory temporary;
    SceneFile file;
    auto state = scene();
    const auto source = temporary.path / "source.vscene";
    REQUIRE(file.save_as(source, state));
    const auto original = bytes(source);
    const auto link = temporary.path / "link.vscene";
    fs::create_symlink(source, link);
    CHECK_FALSE(file.load(link));
    CHECK_FALSE(file.save_as(link, state));
    CHECK_FALSE(file.save_as(link, state, true));
    CHECK(fs::is_symlink(link));
    CHECK(bytes(source) == original);
    const auto dangling = temporary.path / "dangling.vscene";
    fs::create_symlink(temporary.path / "absent", dangling);
    CHECK_FALSE(file.load(dangling));
    CHECK_FALSE(file.save_as(dangling, state, true));
    CHECK_FALSE(file.save_as(temporary.path, state, true));
    CHECK_FALSE(file.save_as(temporary.path / "missing" / "scene.vscene", state));
    CHECK_FALSE(file.save_as({}, state));
    CHECK_FALSE(file.save_as(fs::path(std::string("scene\0other", 11)), state));
    const auto fifo = temporary.path / "fifo.vscene";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK_FALSE(file.load(fifo)); // Must not block trying to read a pipe.
    CHECK_FALSE(file.save_as(fifo, state, true));
    CHECK(file.path() == std::optional{source});
    fs::remove(source);
    fs::create_symlink(link, source);
    CHECK_FALSE(file.save(state)); // A symlink introduced at the tracked path is still rejected.
    CHECK(fs::is_symlink(source));
    no_temporaries(temporary.path);
}

TEST_CASE("Tracked scene paths are absolute and parent aliases resolve once", "[editor][file]") {
    TemporaryDirectory temporary;
    const auto real = temporary.path / "real";
    const auto other = temporary.path / "other";
    fs::create_directory(real);
    fs::create_directory(other);
    const auto alias = temporary.path / "alias";
    fs::create_directory_symlink(real, alias);
    SceneFile file;
    auto state = scene();
    {
        CurrentDirectory cwd{temporary.path};
        REQUIRE(file.save_as("alias/scene.vscene", state));
    }
    CHECK(file.path() == std::optional{real / "scene.vscene"});
    fs::remove(alias);
    fs::create_directory_symlink(other, alias);
    ++state.document.revision;
    (*editor_example::sun_settings(state, 2)).radius = 3;
    {
        CurrentDirectory cwd{other};
        REQUIRE(file.save(state));
    }
    CHECK(bytes(real / "scene.vscene") == encoded(state));
    CHECK_FALSE(fs::exists(other / "scene.vscene"));
    no_temporaries(temporary.path);
}

TEST_CASE("Concurrent create-only Save As operations cannot overwrite the winning scene",
          "[editor][file]") {
    TemporaryDirectory temporary;
    const auto first = scene();
    auto second = first;
    (*editor_example::sun_settings(second, 2)).bloom = 0.8F;
    const auto first_bytes = encoded(first), second_bytes = encoded(second);
    for (unsigned trial = 0; trial < 8; ++trial) {
        const auto destination = temporary.path / ("raced-" + std::to_string(trial) + ".vscene");
        std::barrier start{3};
        std::atomic<unsigned> successes{};
        auto save = [&](const State& state) {
            SceneFile file;
            start.arrive_and_wait();
            if (file.save_as(destination, state))
                ++successes;
        };
        std::thread a{save, std::cref(first)}, b{save, std::cref(second)};
        start.arrive_and_wait();
        a.join();
        b.join();
        CHECK(successes.load() == 1);
        const auto stored = bytes(destination);
        CHECK((stored == first_bytes || stored == second_bytes));
    }
    no_temporaries(temporary.path);
}

TEST_CASE("Scene Save and undo retain timeline values and incoming interpolation",
          "[editor][timeline][file]") {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "animated.vscene";
    auto state = scene();
    REQUIRE(key_property(state, {1, "position"}, 5, Vec3{3, 1, 0}));
    REQUIRE(key_property(state, {2, "visible"}, 6, false));
    SceneFile file;
    REQUIRE(file.save_as(path, state));
    auto loaded = file.load(path);
    REQUIRE(loaded);
    CHECK(loaded->document.timeline == state.document.timeline);
    CHECK(evaluate_scene(*loaded, 2) == evaluate_scene(state, 2));
    const auto original = state.document.timeline;
    EditingSession editing{std::move(state)}; editing.select_keyframe(editing.state().viewport.time);
    auto values = keyframe_values(editing.state(), 5);
    for (auto& field : values) if (field.target == timeline::Target{1, "position"}) {
        field.value = Vec3{2, 0, 0}; field.incoming = timeline::Interpolation::hold;
    }
    REQUIRE(editing.update_keyframe(5, 5, "", values));
    REQUIRE(file.save(editing.state()));
    loaded = file.load(path);
    REQUIRE(loaded);
    CHECK(loaded->document.timeline == editing.state().document.timeline);
    REQUIRE(editing.undo());
    CHECK(editing.state().document.timeline == original);
    REQUIRE(file.save(editing.state()));
    loaded = file.load(path);
    REQUIRE(loaded);
    CHECK(loaded->document.timeline == original);
    REQUIRE(editing.redo());
    CHECK(editing.state().document.timeline != original);
}

TEST_CASE("A full spaceship imports as a retained mesh asset and survives scene disk round trips",
          "[editor][scene-file][import]") {
    TemporaryDirectory temporary;
    auto state = scene();
    const auto builtin = state.document.mesh.document();
    const auto source = fs::path(VNG_TIMELINE_SCENE_PATH).parent_path() / "spaceship.vmesh";
    const auto imported = import_mesh(state, source);
    INFO((imported ? "spaceship imported" : imported.error().message));
    REQUIRE(imported);
    const auto blueprint = find_instance(state, *imported)->blueprint;
    REQUIRE(state.document.mesh_assets.size() == 1);
    REQUIRE(editable_mesh(state)->size() > 20000);
    state.viewport.selected_vertex = 20000;
    REQUIRE(editable_mesh(state)->set_position(20000, {3, 2, 1}));
    const auto imported_document = editable_mesh(state)->document();
    SceneFile file;
    const auto path = temporary.path / "spaceship.vscene";
    REQUIRE(file.save_as(path, state));
    REQUIRE(fs::file_size(path) > 1024 * 1024);
    auto restored = file.load(path);
    REQUIRE(restored);
    CHECK(restored->document.mesh.document() == builtin);
    CHECK(editable_mesh(*restored)->document() == imported_document);
    CHECK(restored->viewport.selected_vertex == 20000);
    EditingSession editing{std::move(*restored)}; editing.select_keyframe(editing.state().viewport.time);
    REQUIRE(editing.erase_instances(std::array<u32,1>{*imported}));
    CHECK(editing.state().viewport.selected_vertex == 0);
    CHECK(mesh_geometry(editing.state(), blueprint));
    REQUIRE(file.save(editing.state()));
    auto without_instance = file.load(path);
    REQUIRE(without_instance);
    CHECK(mesh_geometry(*without_instance, blueprint)->document() == imported_document);
    REQUIRE(editing.undo());
    CHECK(editing.state().viewport.selected_vertex == 20000);
    CHECK(editable_mesh(editing.state())->document() == imported_document);
}
