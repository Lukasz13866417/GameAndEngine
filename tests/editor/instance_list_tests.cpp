#include "../../examples/editor/instance_list.hpp"
#include "../../examples/editor/blueprint_list.hpp"
#include "../../examples/editor/panel_flyout.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace vng;
using namespace editor_example;

TEST_CASE("Floating lists retain shared IDs and independent scrolling without owning scene data",
          "[editor][ui][selection][flyout]") {
    auto font = text::Font::load(VNG_TEST_FONT_PATH); REQUIRE(font);
    ui::Screen screen{ui::dark_theme(*font)};
    auto dock = screen.column().position({750, 100}).width(300).height(208).padding(0)
        .scrollbar(ui::ScrollBar::always);
    PanelFlyout flyout{screen.column(), "INSTANCES", "Close list"};
    const ui::Rect opener{50, 20, 160, 36};
    flyout.layout({1100, 900}, opener);
    InstanceList a{dock}, b{flyout.body()};
    std::vector<SceneInstance> instances;
    for (u32 id=1; id<=40; ++id)
        instances.push_back({.id=id,.name="Instance " + std::to_string(id),.settings=MeshSettings{}});
    const std::array blueprints{Blueprint{BlueprintId::mesh,"Cube",BlueprintKind::mesh}};
    a.sync(instances, blueprints); b.sync(instances, blueprints);
    editor::Selection<u32> selected;
    selected.select(32); a.selection(selected); b.selection(selected);
    input::Frame frame{.logical_size={1100,900},.framebuffer={1100,900}};
    auto pump = [&] { REQUIRE(screen.update(frame, .016F)); };
    pump();
    CHECK_FALSE(flyout.opened());
    flyout.open(); pump();
    b.reveal(32); pump();
    REQUIRE(a.entries().size()==40); REQUIRE(b.entries().size()==40);
    const auto tree = screen.inspect(); REQUIRE(tree);
    const auto dock_button = std::ranges::find_if(tree->widgets, [](const auto& widget) {
        return widget.role == ui::WidgetRole::button && widget.text == "> #32 Instance 32" && widget.bounds.x >= 750;
    });
    const auto float_button = std::ranges::find_if(tree->widgets, [](const auto& widget) {
        return widget.role == ui::WidgetRole::button && widget.text == "> #32 Instance 32" && widget.bounds.x < 750;
    });
    REQUIRE(dock_button != tree->widgets.end()); REQUIRE(float_button != tree->widgets.end());
    CHECK_FALSE(dock_button->visible); CHECK(float_button->visible);
    CHECK(dock_button->text == float_button->text); // Same selected instance.
    CHECK(a.stats().syncs == 1); CHECK(b.stats().syncs == 1);
    const Vec2 point{float_button->clip.x + 10, float_button->clip.y + 10};
    frame.events = {{.kind=input::EventKind::pointer_down,.position=point,.modifiers={.control=true}},
                    {.kind=input::EventKind::pointer_up,.position=point,.modifiers={.control=true}}};
    frame.pointer = point; pump();
    const auto picked = b.poll(frame.events); REQUIRE(picked);
    CHECK(picked->id == 32); CHECK(picked->mode == editor::SelectionMode::toggle);
    CHECK_FALSE(a.poll(frame.events));
    // Closing/reopening retains widgets and independent scroll; deletion is
    // reconciled by stable IDs, not list positions or borrowed record pointers.
    flyout.close(); CHECK_FALSE(flyout.contains(point));
    instances.erase(instances.begin()+31);
    a.sync(instances, blueprints); b.sync(instances, blueprints);
    CHECK_FALSE(a.contains(32)); CHECK_FALSE(b.contains(32));
    flyout.open(); frame.events.clear(); pump();
    const std::array escape{input::Event{.kind=input::EventKind::key_down,.key=input::Key::escape}};
    flyout.poll(escape, opener, true); CHECK_FALSE(flyout.opened());
    flyout.open(); flyout.poll({}, opener, false); CHECK_FALSE(flyout.opened());
}

TEST_CASE("Region lists are filtered views of the same instance identity", "[editor][ui][region][selection]") {
    auto font=text::Font::load(VNG_TEST_FONT_PATH);REQUIRE(font);
    ui::Screen screen{ui::dark_theme(*font)};
    InstanceList scene{screen.column(),InstanceList::Filter::scene},regions{screen.column(),InstanceList::Filter::regions};
    std::vector<SceneInstance> instances{
        {.id=1,.blueprint=BlueprintId::mesh,.name="Ship"},
        {.id=2,.blueprint=BlueprintId::region,.name="TODO",.settings=RegionSettings{}}};
    scene.sync(instances,{});regions.sync(instances,{});
    CHECK(scene.entries().size()==1);CHECK(regions.entries().size()==1);
    CHECK(scene.contains(1));CHECK_FALSE(scene.contains(2));CHECK(regions.contains(2));
    editor::Selection<u32> selection;selection.select(2);
    scene.selection(selection);regions.selection(selection);
    REQUIRE(screen.update(input::Frame{.logical_size={1100,900},.framebuffer={1100,900}},.016F));
    auto tree=screen.inspect();REQUIRE(tree);
    CHECK(std::ranges::any_of(tree->widgets,[](const auto& widget){return widget.text.starts_with("> #2");}));
    instances.pop_back();regions.sync(instances,{});CHECK(regions.entries().empty());
}

TEST_CASE("Instance list selection changes update only affected rows without document synchronization",
          "[editor][ui][selection]") {
    auto font=text::Font::load(VNG_TEST_FONT_PATH);REQUIRE(font);
    ui::Screen screen{ui::dark_theme(*font)};
    InstanceList list{screen.column()};
    std::vector<SceneInstance> instances;
    for(u32 id=1;id<=1000;++id)instances.push_back(SceneInstance{.id=id,.blueprint=BlueprintId::mesh,.name="Instance"});
    list.sync(instances,{});
    CHECK(list.stats().rows_created==1000);
    const auto baseline=list.stats();
    editor::Selection<u32> selected;selected.select(15);
    list.selection(selected);CHECK(list.stats().labels_updated==baseline.labels_updated+1);
    list.selection(selected);CHECK(list.stats().labels_updated==baseline.labels_updated+1);
    selected.select(32);list.selection(selected);
    CHECK(list.stats().labels_updated==baseline.labels_updated+3);
    selected.clear();list.selection(selected);
    CHECK(list.stats().labels_updated==baseline.labels_updated+4);
    list.selection(selected);CHECK(list.stats().labels_updated==baseline.labels_updated+4);
    CHECK(list.stats().syncs==baseline.syncs);
    CHECK(list.stats().rows_created==1000);
    list.sync(instances,{});CHECK(list.stats().labels_updated==baseline.labels_updated+4);
    instances[0].name="Renamed";list.sync(instances,{});
    CHECK(list.stats().labels_updated==baseline.labels_updated+5);
    instances.erase(instances.begin());list.sync(instances,{});
    CHECK_FALSE(list.contains(1));CHECK(list.contains(1000));CHECK(list.entries().size()==999);
    const auto before_rename=list.stats();
    CHECK(list.rename(1000,"Region instance"));CHECK_FALSE(list.rename(1000,"Region instance"));
    CHECK_FALSE(list.rename(9999,"Missing"));
    CHECK(list.stats().labels_updated==before_rename.labels_updated+1);
    CHECK(list.stats().syncs==before_rename.syncs);
    CHECK(list.entries().back().name=="Region instance");
}
