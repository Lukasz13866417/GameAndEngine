#include "../../examples/editor/effect_presets.hpp"
#include "../../examples/editor/editing_session.hpp"
#include "../../examples/editor/animation.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {
using namespace vng;
using namespace editor_example;
struct Directory {
    std::filesystem::path path;
    Directory() {
        char pattern[] = "/tmp/vng-effect-preset-XXXXXX";
        REQUIRE(::mkdtemp(pattern)); path = pattern;
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path,error); }
    std::filesystem::path file(std::string_view text, std::string_view name = "test.veffect") {
        const auto result=path/name; REQUIRE(save_new(result,text)); return result;
    }
};
State initial() {
    auto mesh=editor::EditableMesh::load(std::filesystem::path(VNG_TIMELINE_SCENE_PATH).parent_path()/"colored_cube.vmesh");
    REQUIRE(mesh); return {.document={.mesh=std::move(*mesh)}};
}
std::string encoded_preset() {
    auto text=encode_effect_preset({"Warm \"sun\" / słoneczny",{1.3F,.4F,.12F,false,true}});
    REQUIRE(text); return *text;
}
std::string serialized(const State& state) {
    auto bytes=encode(state); REQUIRE(bytes); return *bytes;
}
}

TEST_CASE("Effect presets round trip named settings using the shared document format", "[editor][effect-preset]") {
    Directory files;
    auto preset=read_effect_preset(files.file(encoded_preset())); REQUIRE(preset);
    CHECK(preset->name=="Warm \"sun\" / słoneczny");
    CHECK(preset->settings==SunSettings{1.3F,.4F,.12F,false,true});
    CHECK(*encode_effect_preset(*preset)==encoded_preset());
    auto defaults=read_effect_preset(files.file(
        "vscene 1.0\neffect_preset=1; type=\"sun\"; name=\"Default\"; settings={};", "defaults.VEFFECT"));
    REQUIRE(defaults); CHECK(defaults->settings==SunSettings{});
    EffectPreset invalid{"bad\nname",{}};
    CHECK_FALSE(encode_effect_preset(invalid));
    invalid.name="valid"; invalid.settings.radius=std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(encode_effect_preset(invalid));
}

TEST_CASE("Effect preset import creates retained independent blueprints and instances", "[editor][effect-preset]") {
    Directory files;
    const auto source=files.file(encoded_preset(),"sun.VEFFECT");
    EditingSession editing{initial()}; editing.select_keyframe(0);
    const auto original=editing.state().document.instances;
    auto imported=editing.import_asset(source); REQUIRE(imported);
    REQUIRE(editing.state().document.effect_assets.size()==1);
    auto state=editing.state();
    const auto blueprint=find_instance(state,*imported)->blueprint;
    CHECK(blueprint!=BlueprintId::sun);
    CHECK(state.viewport.selected_object==*imported);
    CHECK(state.document.instances.size()==original.size()+1);
    CHECK(state.document.instances[0]==original[0]); CHECK(state.document.instances[1]==original[1]);
    REQUIRE(editing.undo()); CHECK(editing.state().document.effect_assets.empty());
    CHECK(editing.state().document.instances==original);
    REQUIRE(editing.redo()); CHECK(editing.state().document.effect_assets==state.document.effect_assets);
    const auto second=instantiate(state,blueprint); REQUIRE(second);
    std::get<SunSettings>(find_instance(state,*imported)->settings).bloom=.8F;
    CHECK(std::get<SunSettings>(find_instance(state,*second)->settings).bloom==.12F);
    CHECK(effect_blueprint_settings(state,blueprint)->bloom==.12F);
    auto exported=effect_preset(state,*imported); REQUIRE(exported);
    CHECK(exported->settings.bloom==.8F);
    REQUIRE(key_property(state,{*imported,"bloom"},0,.2F));
    REQUIRE(key_property(state,{*imported,"bloom"},2,.6F));
    state.viewport.time=1;
    exported=effect_preset(state,*imported); REQUIRE(exported);
    CHECK(std::abs(exported->settings.bloom-.4F)<.00001F);
    CHECK(std::get<SunSettings>(find_instance(state,*imported)->settings).bloom==.8F);
    CHECK_FALSE(effect_preset(state,1)); // Meshes aren't effect presets.
    REQUIRE(erase_instance(state,*imported)); REQUIRE(erase_instance(state,*second));
    REQUIRE(effect_blueprint_settings(state,blueprint));
    auto bytes=encode_scene(state); REQUIRE(bytes);
    REQUIRE(std::filesystem::remove(source));
    auto restored=decode(*bytes); REQUIRE(restored);
    CHECK(restored->document.effect_assets==state.document.effect_assets);
    auto recreated=instantiate(*restored,blueprint); REQUIRE(recreated);
    CHECK(std::get<SunSettings>(find_instance(*restored,*recreated)->settings).bloom==.12F);
    auto mesh=import_asset(*restored,std::filesystem::path(VNG_TIMELINE_SCENE_PATH).parent_path()/"colored_cube.vmesh");
    REQUIRE(mesh); CHECK(find_instance(*restored,*mesh)->blueprint!=blueprint);
    REQUIRE(encode(*restored));
}

TEST_CASE("Invalid effect imports are atomic and report unsupported types and settings", "[editor][effect-preset]") {
    Directory files;
    auto state=initial(); const auto before=serialized(state);
    unsigned index=0;
    for(const auto text:{
        "vscene 1.0\neffect_preset=1; type=\"unknown\"; name=\"Bad\"; settings={};",
        "vscene 1.0\neffect_preset=2; type=\"sun\"; name=\"Bad\"; settings={};",
        "vscene 1.0\neffect_preset=1; type=\"sun\"; name=\"Bad\"; settings={radius=-1;};",
        "vscene 1.0\neffect_preset=1; type=\"sun\"; name=\"Bad\"; settings={bloom=\"bad\";};",
        "not a preset"}) {
        const auto path=files.file(text,std::to_string(index++)+".veffect");
        auto imported=import_asset(state,path); CHECK_FALSE(imported);
        CHECK(serialized(state)==before);
    }
    CHECK_FALSE(import_asset(state,files.path/"missing.veffect"));
    CHECK_FALSE(import_asset(state,files.file(encoded_preset(),"unsupported.txt")));
    CHECK(serialized(state)==before);
}

TEST_CASE("Scene validation rejects ambiguous effect blueprint identities", "[editor][effect-preset]") {
    Directory files; auto state=initial();
    REQUIRE(import_asset(state,files.file(encoded_preset())));
    const auto asset=state.document.effect_assets.front();
    state.document.effect_assets.push_back(asset); CHECK_FALSE(encode(state));
    state.document.effect_assets.pop_back();
    state.document.effect_assets.front().id=BlueprintId::sun; CHECK_FALSE(encode(state));
    state.document.effect_assets.front()=asset;
    state.document.effect_assets.front().settings.displacement=2; CHECK_FALSE(encode(state));
}
