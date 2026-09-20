#include "../../examples/editor/edit_shortcuts.hpp"
#include "../../examples/editor/edit_clipboard.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace vng;
using namespace editor_example;
namespace {
State scene() {
    content::vmesh::Document document;
    document.vertex_count=3;
    document.vertex_fields={{"position",{content::vmesh::ScalarType::Float32,3},std::vector<f32>(9)}};
    document.faces={{0,1,2}};
    auto mesh=editor::EditableMesh::create(std::move(document)); REQUIRE(mesh);
    return {.document={.mesh=std::move(*mesh)}};
}
}
TEST_CASE("Edit shortcuts respect UI consumption, modifiers, repetition and focus", "[editor][shortcuts]") {
    std::vector<input::Event> raw{{.kind=input::EventKind::key_down,.key=input::Key::c,.modifiers={.control=true}}};
    CHECK(edit_shortcuts(raw,raw,true)==std::vector{EditShortcut::copy});
    CHECK(edit_shortcuts({},raw,true).empty()); // Text field consumed Ctrl+C.
    CHECK(edit_shortcuts(raw,raw,false).empty());
    raw[0].key=input::Key::v; CHECK(edit_shortcuts(raw,raw,true)==std::vector{EditShortcut::paste});
    raw[0].key=input::Key::z; CHECK(edit_shortcuts(raw,raw,true)==std::vector{EditShortcut::undo});
    raw[0].modifiers.shift=true; CHECK(edit_shortcuts(raw,raw,true)==std::vector{EditShortcut::redo});
    raw[0].key=input::Key::y; raw[0].modifiers.shift=false; CHECK(edit_shortcuts(raw,raw,true)==std::vector{EditShortcut::redo});
    raw[0].repeat=true; CHECK(edit_shortcuts(raw,raw,true).empty());
    raw[0].repeat=false; raw.push_back({.kind=input::EventKind::focus_lost});
    CHECK(edit_shortcuts(raw,raw,true).empty());
}
TEST_CASE("Edit shortcut bursts preserve every action in arrival order", "[editor][shortcuts]") {
    std::vector<input::Event> raw;
    for (const auto key : {input::Key::c, input::Key::v, input::Key::v, input::Key::z, input::Key::y}) {
        raw.push_back({.kind=input::EventKind::key_down, .key=key, .modifiers={.control=true}});
        raw.push_back({.kind=input::EventKind::key_up, .key=key, .modifiers={.control=true}});
    }
    CHECK(edit_shortcuts(raw,raw,true) == std::vector{
        EditShortcut::copy, EditShortcut::paste, EditShortcut::paste, EditShortcut::undo, EditShortcut::redo});
    // A text field's consumed copy must not turn into a document copy.
    CHECK(edit_shortcuts(std::span{raw}.subspan(2), raw, true) == std::vector{
        EditShortcut::paste, EditShortcut::paste, EditShortcut::undo, EditShortcut::redo});
    raw.back() = {.kind=input::EventKind::pointer_down};
    CHECK(edit_shortcuts(raw,raw,true).empty()); // Do not target a same-batch selection ambiguously.
}
TEST_CASE("Instance clipboard shares blueprint and independently retargets animation", "[editor][clipboard]") {
    auto state=scene(); EditClipboard clipboard;
    REQUIRE(key_property(state,{1,"scale"},0,.5F));
    REQUIRE(clipboard.copy_instance(state,1));
    const auto copied=clipboard.paste(state); REQUIRE(copied); REQUIRE(copied->object);
    CHECK(*copied->object!=1); CHECK(state.document.instances.size()==3);
    CHECK(find_instance(state,*copied->object)->blueprint==BlueprintId::mesh);
    REQUIRE(state.document.timeline.find({*copied->object,"scale"}));
    REQUIRE(key_property(state,{*copied->object,"scale"},0,2.F));
    CHECK(state.document.timeline.sample({1,"scale"},0)==timeline::Value{.5F});
    REQUIRE(erase_instance(state,1)); REQUIRE(clipboard.paste(state));
    clipboard.clear(); CHECK_FALSE(clipboard.paste(state));
}
TEST_CASE("Keyframe clipboard preserves sparse values, names and interpolation", "[editor][clipboard]") {
    auto state=scene(); EditClipboard clipboard;
    REQUIRE(key_property(state,{1,"scale"},1,2.F,timeline::Interpolation::hold));
    state.document.keyframe_names[1]="Arrival";
    REQUIRE(clipboard.copy_keyframe(state,1));
    state.viewport.time=5;
    const auto pasted=clipboard.paste(state); REQUIRE(pasted); CHECK(pasted->keyframe==5.F);
    CHECK(keyframe_name(state,5)=="Arrival");
    CHECK(state.document.timeline.find({1,"scale"})->keys.back().incoming==timeline::Interpolation::hold);
    CHECK_FALSE(state.document.timeline.find({1,"position"}));
    CHECK_FALSE(clipboard.paste(state)); // No implicit overwrite of existing keys.
    REQUIRE(erase_instance(state,1)); state.viewport.time=6;
    CHECK_FALSE(clipboard.paste(state)); CHECK_FALSE(state.document.keyframe_names.contains(6));
}
