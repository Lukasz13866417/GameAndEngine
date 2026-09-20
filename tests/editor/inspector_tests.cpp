#include <vng/editor/inspector.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <memory>
#include <stdexcept>

namespace {
using namespace vng;
namespace editor = vng::editor;

struct Settings final {
    f32 emission{3.0F};
    bool white_spots{};
    i32 passes{2};
    u32 seed{42};
    Vec3 position{1.0F, 2.0F, 3.0F};
    std::string name{"Sun"};
    int hidden_runtime_setting{97};
    friend bool operator==(const Settings&, const Settings&) = default;
};

editor::Event event(const editor::Inspector& inspector, std::string key,
    editor::Phase phase = editor::Phase::apply, std::vector<editor::NamedValue> values = {})
{ return {inspector.schema().stamp, std::move(key), phase, std::move(values)}; }
} // namespace

TEST_CASE("Translation handles are descriptions, not extra editable callback arguments", "[editor][gizmo]") {
    editor::Inspector inspector{1, 2, 3};
    Vec3 position{};
    auto gizmo = inspector.translation_gizmo("move", position, [&](Vec3 next) { position = next; });
    gizmo.axis("Forward / back", {0, 0, -5});
    CHECK_THROWS_AS(gizmo.axis("Zero", {}), std::invalid_argument);
    CHECK_THROWS_AS(gizmo.axis("Invalid", {std::numeric_limits<f32>::infinity(), 0, 0}), std::invalid_argument);
    CHECK_THROWS_AS(gizmo.axis("", {1, 0, 0}), std::invalid_argument);
    CHECK_THROWS_AS(gizmo.axis("Forward / back", {1, 0, 0}), std::invalid_argument);
    REQUIRE(editor::validate(inspector.schema()));
    auto bad = inspector.schema();
    bad.controls[0].translation_axes[0].direction = {};
    CHECK_FALSE(editor::encode_schema(bad));
    bad = inspector.schema();
    bad.controls[0].translation_axes.push_back(bad.controls[0].translation_axes[0]);
    CHECK_FALSE(editor::encode_schema(bad));
    bad = inspector.schema();
    bad.controls[0].translation_axes.resize(9, {"axis", {1,0,0}});
    CHECK_FALSE(editor::encode_schema(bad));
    // The wire event remains the existing position edit. No axis index, local
    // object pointer or extra field can be smuggled into a callback.
    CHECK_FALSE(inspector.dispatch(event(inspector, "move", editor::Phase::apply,
        {{"direction", Vec3{1,0,0}}})));
    REQUIRE(inspector.dispatch(event(inspector, "move", editor::Phase::apply,
        {{"position", Vec3{0,0,-2}}})));
    CHECK(position == Vec3{0,0,-2});
    CHECK_THROWS_AS(gizmo.axis("Late", {1,0,0}), std::logic_error);
}

TEST_CASE("Inspector exposes ordinary settings without serializing native layouts", "[editor][inspector]")
{
    Settings settings;
    editor::Inspector inspector{12, 5, 8};
    int calls{};
    auto edit = inspector.edit("sun_settings", settings);
    edit.slider("emission", &Settings::emission, 0, 20)
        .toggle("white_spots", &Settings::white_spots)
        .field("passes", &Settings::passes)
        .field("seed", &Settings::seed)
        .field("position", &Settings::position)
        .field("name", &Settings::name);
    edit.apply("Apply", [&](const Settings& next) { settings = next; ++calls; });
    REQUIRE(editor::validate(inspector.schema()));
    REQUIRE(inspector.schema().controls.size() == 1);
    CHECK(inspector.schema().controls[0].label == "Sun Settings");
    CHECK(inspector.schema().controls[0].fields.size() == 6);
    CHECK(calls == 0);

    const auto first = event(inspector, "sun_settings", editor::Phase::apply,
        {{"emission", 8.0F}, {"white_spots", true}});
    REQUIRE(inspector.dispatch(first));
    CHECK(settings.emission == 8.0F);
    CHECK(settings.white_spots);
    CHECK(settings.hidden_runtime_setting == 97);
    CHECK(calls == 1);
    CHECK(inspector.schema().stamp == editor::Stamp{12, 5, 9});
    CHECK(std::get<f32>(inspector.schema().controls[0].fields[0].value) == 8.0F);
    CHECK_FALSE(inspector.dispatch(first)); // Replay cannot invoke the callback twice.
    CHECK(calls == 1);

    REQUIRE(inspector.dispatch(event(inspector, "sun_settings", editor::Phase::apply,
        {{"name", std::string("Quiet Sun")}})));
    CHECK(settings.name == "Quiet Sun");
    CHECK(settings.emission == 8.0F); // Partial patches preserve the accepted baseline.
    CHECK(settings.white_spots);
    CHECK(calls == 2);
    CHECK_THROWS_AS(edit.field("later", &Settings::name), std::logic_error);
}

TEST_CASE("Inspector validates complete patches before invoking user code", "[editor][inspector]")
{
    Settings settings;
    editor::Inspector inspector{1, 2};
    int calls{};
    auto edit = inspector.edit("settings", settings);
    edit.slider("emission", &Settings::emission, 0, 20);
    edit.toggle("spots", &Settings::white_spots);
    edit.apply("Apply", [&](const Settings& next) -> editor::Result<void> {
        ++calls;
        if (next.emission == 17.0F) return std::unexpected(editor::Diagnostic{"GPU rebuild failed"});
        if (next.emission == 18.0F) throw std::runtime_error("Unexpected rebuild failure");
        settings = next;
        return {};
    });
    auto check_invalid = [&](editor::Event packet) {
        CHECK_FALSE(inspector.dispatch(packet));
        CHECK(calls == 0);
        CHECK(settings.emission == 3.0F);
        CHECK(inspector.schema().stamp.revision == 0);
    };
    check_invalid(event(inspector, "settings", editor::Phase::apply, {{"emission", 21.0F}}));
    check_invalid(event(inspector, "settings", editor::Phase::apply, {{"emission", i32{4}}}));
    check_invalid(event(inspector, "settings", editor::Phase::apply, {{"emission", 4.0F}, {"unknown", true}}));
    check_invalid(event(inspector, "settings", editor::Phase::apply, {{"emission", 4.0F}, {"emission", 5.0F}}));
    check_invalid(event(inspector, "settings", editor::Phase::update, {{"emission", 4.0F}}));
    check_invalid(event(inspector, "missing"));
    check_invalid(event(inspector, "settings", editor::Phase::apply, {{"emission", std::numeric_limits<f32>::quiet_NaN()}}));
    auto wrong_worker = event(inspector, "settings"); ++wrong_worker.stamp.generation; check_invalid(wrong_worker);
    auto wrong_object = event(inspector, "settings"); ++wrong_object.stamp.object; check_invalid(wrong_object);
    auto wrong_revision = event(inspector, "settings"); ++wrong_revision.stamp.revision; check_invalid(wrong_revision);

    auto rejected = inspector.dispatch(event(inspector, "settings", editor::Phase::apply, {{"emission", 17.0F}}));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message == "GPU rebuild failed");
    CHECK(calls == 1);
    CHECK(inspector.schema().stamp.revision == 0);
    CHECK(std::get<f32>(inspector.schema().controls[0].fields[0].value) == 3.0F);
    auto threw = inspector.dispatch(event(inspector, "settings", editor::Phase::apply, {{"emission", 18.0F}}));
    REQUIRE_FALSE(threw);
    CHECK(threw.error().message.find("Unexpected rebuild failure") != std::string::npos);
    CHECK(inspector.schema().stamp.revision == 0);
    REQUIRE(inspector.dispatch(event(inspector, "settings", editor::Phase::apply, {{"spots", true}})));
    CHECK(settings.emission == 3.0F); // Failed candidates never become the next baseline.
    CHECK(settings.white_spots);
}

TEST_CASE("Inspector actions live fields and move-only callbacks retain no foreign pointers", "[editor][inspector]")
{
    Settings settings;
    editor::Inspector inspector{1, 1};
    auto live = inspector.edit("live", settings);
    live.field("name", &Settings::name);
    live.live([&](const Settings& next) -> std::expected<void, std::string> {
        settings = next; return {};
    });
    int called{};
    inspector.action("reset", [owned = std::make_unique<int>(7), &called] { called += *owned; });
    REQUIRE(inspector.dispatch(event(inspector, "live", editor::Phase::update, {{"name", std::string{}}})));
    CHECK(settings.name.empty());
    REQUIRE(inspector.dispatch(event(inspector, "live", editor::Phase::commit, {{"name", std::string{"é"}}})));
    CHECK(settings.name == "é");
    CHECK_FALSE(inspector.dispatch(event(inspector, "reset")));
    REQUIRE(inspector.dispatch(event(inspector, "reset", editor::Phase::activate)));
    CHECK(called == 7);
    auto moved = std::move(inspector);
    REQUIRE(moved.dispatch(event(moved, "reset", editor::Phase::activate)));
    CHECK(called == 14);
    CHECK_THROWS_AS(inspector.schema(), std::logic_error);
    CHECK_FALSE(inspector.dispatch(event(moved, "reset", editor::Phase::activate)));
}

TEST_CASE("Inspector gizmo gestures distinguish preview commit cancel and reject bad ordering", "[editor][inspector]")
{
    editor::Inspector inspector{9, 4};
    Vec3 position{1, 2, 3};
    std::vector<editor::Phase> phases;
    inspector.translation_gizmo("origin", position, [&](Vec3 next, editor::Phase phase) -> editor::Result<void> {
        if (next.x > 100.0F) return std::unexpected(editor::Diagnostic{"Outside world bounds"});
        position = next; phases.push_back(phase); return {};
    });
    auto send = [&](editor::Phase phase, std::vector<editor::NamedValue> values = {}) {
        return inspector.dispatch(event(inspector, "origin", phase, std::move(values)));
    };
    CHECK_FALSE(send(editor::Phase::update, {{"position", Vec3{4, 5, 6}}}));
    CHECK_FALSE(send(editor::Phase::commit));
    REQUIRE(send(editor::Phase::begin));
    CHECK_FALSE(send(editor::Phase::begin));
    REQUIRE(send(editor::Phase::update, {{"position", Vec3{4, 5, 6}}}));
    REQUIRE(send(editor::Phase::update, {{"position", Vec3{7, 8, 9}}}));
    CHECK(position == Vec3{7, 8, 9});
    CHECK_FALSE(send(editor::Phase::update, {{"position", Vec3{101, 0, 0}}}));
    CHECK(position == Vec3{7, 8, 9});
    REQUIRE(send(editor::Phase::cancel));
    CHECK(position == Vec3{1, 2, 3});
    CHECK(std::get<Vec3>(inspector.schema().controls[0].fields[0].value) == position);
    CHECK(phases == std::vector<editor::Phase>{editor::Phase::begin, editor::Phase::update,
        editor::Phase::update, editor::Phase::cancel});
    REQUIRE(send(editor::Phase::begin));
    REQUIRE(send(editor::Phase::commit, {{"position", Vec3{10, 20, 30}}}));
    CHECK(position == Vec3{10, 20, 30});
    REQUIRE(send(editor::Phase::apply, {{"position", Vec3{11, 21, 31}}}));
    CHECK(position == Vec3{11, 21, 31});
}

TEST_CASE("Inspector rejects malformed declarations and reentrant callbacks", "[editor][inspector]")
{
    Settings settings;
    editor::Inspector inspector{1, 1};
    auto edit = inspector.edit("settings", settings);
    CHECK_THROWS_AS(inspector.edit("settings", settings), std::invalid_argument);
    CHECK_THROWS_AS(inspector.edit("bad key", settings), std::invalid_argument);
    CHECK_THROWS_AS(edit.slider("bad_range", &Settings::emission, 10, 0), std::invalid_argument);
    CHECK_THROWS_AS(edit.slider("out_of_range", &Settings::emission, 0, 1), std::invalid_argument);
    edit.toggle("spots", &Settings::white_spots);
    CHECK_THROWS_AS(edit.toggle("spots", &Settings::white_spots), std::invalid_argument);
    CHECK_FALSE(editor::validate(inspector.schema())); // Forgotten apply()/live().
    edit.apply("Apply", [&](const Settings&) {
        CHECK_FALSE(inspector.dispatch(event(inspector, "settings")));
        CHECK_THROWS_AS(inspector.action("late", [] {}), std::logic_error);
    });
    CHECK_THROWS_AS(edit.live([](const Settings&) {}), std::logic_error);
    CHECK_THROWS_AS(edit.field("late", &Settings::name), std::logic_error);
    REQUIRE(inspector.dispatch(event(inspector, "settings")));
    CHECK(inspector.schema().stamp.revision == 1);

    editor::Inspector exhausted{1, 1, std::numeric_limits<u64>::max()};
    exhausted.action("run", [] { FAIL("Must reject exhausted revision before invocation"); });
    CHECK_FALSE(exhausted.dispatch(event(exhausted, "run", editor::Phase::activate)));
}

TEST_CASE("Editor descriptions and events have deterministic bounded versioned wire encodings", "[editor][wire]")
{
    Settings settings;
    editor::Inspector inspector{0x1122334455667788ULL, 4, 9};
    auto edit = inspector.edit("settings", settings);
    edit.slider("emission", &Settings::emission, 0, 20).toggle("spots", &Settings::white_spots)
        .field("signed", &Settings::passes).field("unsigned", &Settings::seed)
        .field("position", &Settings::position).field("name", &Settings::name);
    edit.apply("Apply", [](const Settings&) {});
    inspector.action("reset", [] {});
    inspector.translation_gizmo("position", settings.position, [](Vec3) {})
        .axis("Forward / back", {0, 0, -1}).axis("Lift", {0, 2, 0});
    auto encoded = editor::encode_schema(inspector.schema());
    REQUIRE(encoded);
    CHECK(encoded->substr(0, 5) == std::string("VNGS\x03", 5));
    CHECK(static_cast<u8>((*encoded)[5]) == 0x88U);
    CHECK(editor::encode_schema(inspector.schema()) == encoded);
    auto decoded = editor::decode_schema(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == inspector.schema());
    auto packet = event(inspector, "settings", editor::Phase::apply,
        {{"emission", 7.5F}, {"spots", true}, {"signed", i32{-9}}, {"unsigned", u32{456}},
         {"position", Vec3{1, 2, 3}}, {"name", std::string{"Zażółć"}}});
    auto wire_event = editor::encode_event(packet);
    REQUIRE(wire_event);
    CHECK(editor::decode_event(*wire_event) == editor::Result<editor::Event>{packet});

    for (std::size_t size = 0; size < encoded->size(); ++size)
        CHECK_FALSE(editor::decode_schema(std::string_view(*encoded).substr(0, size)));
    for (std::size_t size = 0; size < wire_event->size(); ++size)
        CHECK_FALSE(editor::decode_event(std::string_view(*wire_event).substr(0, size)));
    CHECK_FALSE(editor::decode_schema(*encoded + "trailing"));
    CHECK_FALSE(editor::decode_event(*wire_event + "trailing"));
    CHECK_FALSE(editor::decode_schema(*wire_event));
    CHECK_FALSE(editor::decode_event(*encoded));
    auto wrong_version = *encoded; wrong_version[4] = 1;
    CHECK_FALSE(editor::decode_schema(wrong_version));
    auto hostile_count = *encoded;
    for (std::size_t at = 37; at < 41; ++at) hostile_count[at] = static_cast<char>(0xff);
    CHECK_FALSE(editor::decode_schema(hostile_count));

    auto invalid = inspector.schema(); invalid.controls.push_back(invalid.controls[0]);
    CHECK_FALSE(editor::encode_schema(invalid));
    packet.values[0].value = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(editor::encode_event(packet));
    packet.values[0].value = std::string(65537, 'a');
    CHECK_FALSE(editor::encode_event(packet));
    packet.values[0].value = std::string("\xF0\x80\x80\x80", 4);
    CHECK_FALSE(editor::encode_event(packet));
    packet.values[0].value = std::string("abc\0def", 7);
    CHECK_FALSE(editor::encode_event(packet));
}
