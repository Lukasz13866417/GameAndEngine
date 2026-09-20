#include "../../examples/editor/toolbar_row.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace vng;
text::Font toolbar_font() {
    auto font=text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(font);
    return *font;
}
struct ToolbarFixture {
    ui::Screen screen{ui::dark_theme(toolbar_font())};
    ui::Container row=screen.row().position({10,10}).width(600);
    editor_example::ToolbarRow toolbar{row,screen.column(),"More..."};
    ui::Button first=toolbar.item(100).button("First");
    ui::TextField draft=toolbar.item(140).text_input("Draft").value("Keep me");
    ui::Button last=toolbar.item(110).button("Last");
    input::Frame frame{.logical_size={640,480},.framebuffer={640,480}};
    void pump(std::initializer_list<input::Event> events={}) {
        toolbar.layout(frame.logical_size);
        frame.events=events;
        for(const auto& event:events) frame.pointer=event.position;
        const auto updated=screen.update(frame,.016F); REQUIRE(updated);
        toolbar.poll(frame.events,true,updated->capturesPointer);
        toolbar.layout(frame.logical_size);
    }
    ui::WidgetSnapshot find(ui::WidgetRole role,std::string_view name) {
        const auto tree=screen.inspect(); REQUIRE(tree);
        const auto found=std::ranges::find_if(tree->widgets,[&](const auto& item){return item.role==role && item.label==name;});
        REQUIRE(found!=tree->widgets.end()); return *found;
    }
    void click(ui::Rect bounds) {
        const Vec2 point{bounds.x+bounds.width*.5F,bounds.y+bounds.height*.5F};
        pump({{.kind=input::EventKind::pointer_down,.position=point},
              {.kind=input::EventKind::pointer_up,.position=point}});
    }
};
}
TEST_CASE("Toolbar overflow keeps one set of widgets and restores their order", "[editor][ui][toolbar]") {
    ToolbarFixture f;f.pump();
    const auto id=f.find(ui::WidgetRole::button,"Last").id;
    CHECK_FALSE(f.find(ui::WidgetRole::button,"More...").visible);
    CHECK(f.first.bounds().x<f.draft.bounds().x);
    CHECK(f.draft.bounds().x<f.last.bounds().x);
    CHECK(f.toolbar.popup_anchor(f.last).x == f.last.bounds().x);
    CHECK(f.toolbar.popup_anchor(f.last).y == f.last.bounds().y);
    f.row.width(260);f.pump();
    CHECK(f.find(ui::WidgetRole::button,"First").visible);
    CHECK_FALSE(f.find(ui::WidgetRole::button,"Last").visible);
    auto more=f.find(ui::WidgetRole::button,"More..."); REQUIRE(more.visible);
    f.click(more.bounds);
    REQUIRE(f.toolbar.opened());
    const auto last=f.find(ui::WidgetRole::button,"Last"); REQUIRE(last.visible);
    CHECK(f.toolbar.popup_anchor(f.last).x == more.bounds.x);
    CHECK(f.toolbar.popup_anchor(f.last).y == more.bounds.y);
    CHECK(last.bounds.y>f.row.bounds().y+f.row.bounds().height);
    f.click(last.bounds); CHECK(f.last.clicked());
    f.toolbar.close();
    CHECK(f.toolbar.popup_anchor(f.last).x == more.bounds.x);
    CHECK(f.toolbar.popup_anchor(f.last).y == more.bounds.y);
    CHECK(f.draft.getText()=="Keep me");
    f.row.width(600);f.pump();
    CHECK_FALSE(f.toolbar.opened());
    CHECK_FALSE(f.find(ui::WidgetRole::button,"More...").visible);
    CHECK(f.find(ui::WidgetRole::button,"Last").id==id);
    CHECK(f.first.bounds().x<f.draft.bounds().x);
    CHECK(f.draft.bounds().x<f.last.bounds().x);
    CHECK(f.first.bounds().y==f.last.bounds().y);
}
TEST_CASE("Toolbar overflow dismisses on outside clicks Escape and modal disabling", "[editor][ui][toolbar]") {
    ToolbarFixture f;f.row.width(260);f.pump();
    for(unsigned how=0;how<3;++how) {
        f.toolbar.enabled(true);f.pump();
        f.click(f.find(ui::WidgetRole::button,"More...").bounds);
        REQUIRE(f.toolbar.opened());
        if(how==0) f.pump({{.kind=input::EventKind::pointer_down,.position={620,450}}});
        if(how==1) f.pump({{.kind=input::EventKind::key_down,.key=input::Key::escape}});
        if(how==2) f.toolbar.enabled(false);
        CHECK_FALSE(f.toolbar.opened());
        CHECK_FALSE(f.find(ui::WidgetRole::button,"Last").visible);
        CHECK(f.draft.getText()=="Keep me");
    }
}
TEST_CASE("Overflow dropdown choices outside the menu survive a captured press and release", "[editor][ui][toolbar]") {
    ToolbarFixture f;
    auto choice=f.toolbar.item(140).dropdown<int>("Choice",{{0,"Zero"},{1,"One"}});
    f.row.width(260);f.pump();
    f.click(f.find(ui::WidgetRole::button,"More...").bounds);
    f.click(choice.bounds());
    const auto option=f.find(ui::WidgetRole::option,"One");
    REQUIRE(option.visible);
    const Vec2 point{option.bounds.x+20,option.bounds.y+option.bounds.height*.5F};
    f.pump({{.kind=input::EventKind::pointer_down,.position=point}});
    REQUIRE(f.toolbar.opened());
    f.pump({{.kind=input::EventKind::pointer_up,.position=point}});
    CHECK(choice.value()==1);
    REQUIRE(choice.changedValue());
    CHECK(*choice.changedValue()==1);
}
