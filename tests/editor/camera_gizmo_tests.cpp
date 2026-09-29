#include "../../examples/editor/camera_gizmo.hpp"
#include <vng/ui/inspection.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>

namespace {
using namespace vng;
using namespace editor_example;
using input::EventKind;
using input::Key;
struct Motion {
    CameraGizmo gizmo;
    input::Frame raw{.logical_size={900,700},.framebuffer={900,700},.focused=true};
    NavigationFrame frame{.pose={0,0,8,{},1},.mode=ViewMode::scene,.viewport={0,0,900,700},
        .raw=raw,.unhandled={},.seconds=.02,.drag_speeds={},.walk_speeds={},.move_forward=true};
    NavigationReply send(std::initializer_list<input::Event> events={},bool available=true,bool enabled=true) {
        raw.events=events;
        frame.unhandled=available?std::span<const input::Event>{raw.events}:std::span<const input::Event>{};
        auto reply=gizmo.update(frame,enabled);frame.pose=reply.pose;return reply;
    }
    void drag(u32 button=0,input::Modifiers modifiers={}) {
        send({{.kind=EventKind::pointer_down,.position={100,100},.button=button,.modifiers=modifiers},
              {.kind=EventKind::pointer_move,.position={120,130},.modifiers=modifiers},
              {.kind=EventKind::pointer_up,.position={120,130},.button=button,.modifiers=modifiers}});
    }
};
text::Font font() {auto loaded=text::Font::load(VNG_TEST_FONT_PATH);REQUIRE(loaded);return *loaded;}
struct Menu {
    ui::Screen screen{ui::dark_theme(font())};
    CameraGizmo gizmo{screen.column()};
    Settings settings;
    CameraGizmo::Presentation presentation{{20,30,900,700}};
    input::Frame raw{.logical_size={1000,800},.framebuffer={1000,800},.focused=true};
    ui::UpdateResult unused;
    CameraPreferenceEdit pump(std::initializer_list<input::Event> events={}) {
        gizmo.present(presentation,settings);
        raw.events=events;
        for(const auto& event:events)if(event.kind==EventKind::pointer_move||event.kind==EventKind::pointer_down||event.kind==EventKind::pointer_up||event.kind==EventKind::scroll)raw.pointer=event.position;
        auto result=screen.update(raw,.016F);REQUIRE(result);unused=*result;
        auto reply=gizmo.poll(settings);if(reply.changed)settings=reply.value;
        REQUIRE(screen.draw_list());return reply;
    }
    ui::WidgetSnapshot widget(ui::WidgetRole role,std::string_view label) {
        auto tree=screen.inspect();REQUIRE(tree);
        for(const auto& w:tree->widgets)if(w.role==role&&w.label==label)return w;
        FAIL("Missing control: "<<label);return {};
    }
    ui::WidgetSnapshot reveal(ui::WidgetRole role,std::string_view label) {
        for(unsigned i=0;i<30;++i) {
            auto w=widget(role,label);
            if(w.visible&&w.clip.height>=w.bounds.height)return w;
            const auto b=gizmo.bounds();
            pump({{.kind=EventKind::scroll,.position={b.x+50,b.y+80},.scroll={0,w.bounds.y<b.y?3.F:-3.F}}});
        }
        FAIL("Could not scroll to control: "<<label);return {};
    }
    CameraPreferenceEdit click(const ui::WidgetSnapshot& w,float x=.5F) {
        const Vec2 p{w.bounds.x+w.bounds.width*x,w.bounds.y+w.bounds.height*.5F};
        return pump({{.kind=EventKind::pointer_down,.position=p},{.kind=EventKind::pointer_up,.position=p}});
    }
};
}

TEST_CASE("Camera gizmo defaults to a rendered camera target without stealing object clicks", "[editor][camera_gizmo]") {
    Motion m;const auto before=m.frame.pose;
    CHECK(m.gizmo.active());CHECK_FALSE(m.gizmo.exclusive());
    m.drag();CHECK(m.frame.pose==before);CHECK_FALSE(m.gizmo.handledPointer());
    m.gizmo.present({.viewport=m.frame.viewport,.other_gizmo=true},{});
    CHECK_FALSE(m.gizmo.active());
    m.drag(2);CHECK(m.frame.pose.yaw!=before.yaw);CHECK(m.gizmo.handledPointer());
    CHECK_FALSE(m.gizmo.exclusive());CHECK_FALSE(m.gizmo.dragging());
}
TEST_CASE("Camera gizmo children each implement their own navigation operation", "[editor][camera_gizmo]") {
    for(const auto& description:camera_gizmo_descriptions) {
        if(description.mode==CameraGizmoMode::walk)continue;
        CAPTURE(description.label);
        Motion m;const auto before=m.frame.pose;const auto eye=camera(before,ViewMode::scene).position();
        m.gizmo.select(description.mode);m.drag();
        const auto after=m.frame.pose;CHECK(after!=before);CHECK(m.gizmo.exclusive());
        CHECK_FALSE(m.gizmo.dragging());CHECK(m.gizmo.mode()==description.mode);
        if(description.mode==CameraGizmoMode::look) {
            const auto next_eye=camera(after,ViewMode::scene).position();
            for(unsigned i=0;i<3;++i)CHECK(next_eye[i]==Catch::Approx(eye[i]).margin(1e-5));
        } else if(description.mode==CameraGizmoMode::orbit)CHECK(after.target==before.target);
        else {CHECK(after.yaw==before.yaw);CHECK(after.pitch==before.pitch);}
        if(description.mode==CameraGizmoMode::zoom){CHECK(after.zoom!=before.zoom);CHECK(after.target==before.target);}
        else CHECK(after.zoom==before.zoom);
    }
}
TEST_CASE("Camera shortcut children can borrow input without replacing an explicit mode", "[editor][camera_gizmo]") {
    Motion m;m.gizmo.select(CameraGizmoMode::look);const auto initial=m.frame.pose;
    m.drag(2,{.shift=true});CHECK(m.frame.pose.target!=initial.target);CHECK(m.frame.pose.yaw==initial.yaw);
    CHECK(m.gizmo.mode()==CameraGizmoMode::look);
    const auto panned=m.frame.pose;
    m.drag(2,{.control=true});CHECK(m.frame.pose.target!=panned.target);CHECK(m.frame.pose.zoom==panned.zoom);
    m.send({{.kind=EventKind::key_down,.key=Key::escape}});CHECK_FALSE(m.gizmo.exclusive());
    m.frame.orbit_enabled=false;const auto blocked=m.frame.pose;
    m.drag(2);CHECK(m.frame.pose==blocked); // Free-rotation object gizmo owns plain MMB.
    m.drag(2,{.shift=true});CHECK(m.frame.pose!=blocked); // But can still pan the camera.
}
TEST_CASE("Camera child capture survives UI consumption and ends only on its own button release", "[editor][camera_gizmo]") {
    Motion m;m.gizmo.select(CameraGizmoMode::pan);
    m.send({{.kind=EventKind::pointer_down,.position={100,100}}});REQUIRE(m.gizmo.dragging());
    m.send({{.kind=EventKind::pointer_up,.position={120,100},.button=2}});CHECK(m.gizmo.dragging());
    auto reply=m.send({{.kind=EventKind::pointer_move,.position={130,100}}},false);CHECK(reply.changed);
    m.send({{.kind=EventKind::pointer_up,.position={130,100}}},false);CHECK_FALSE(m.gizmo.dragging());
    m.send({{.kind=EventKind::pointer_down,.position={100,100}}});
    m.frame.viewport.width+=20;reply=m.send();CHECK(reply.cancelled);CHECK_FALSE(m.gizmo.dragging());
    m.send({{.kind=EventKind::pointer_down,.position={100,100}}});
    reply=m.send({},true,false);CHECK(reply.cancelled);CHECK_FALSE(m.gizmo.dragging());
}
TEST_CASE("Active camera gizmo cycles its children with either Control key", "[editor][camera_gizmo]") {
    Motion m;
    m.send({{.kind=EventKind::key_down,.key=Key::right,.modifiers={.control=true}}});
    CHECK(m.gizmo.mode()==CameraGizmoMode::look);CHECK(m.gizmo.exclusive());
    m.send({{.kind=EventKind::key_down,.key=Key::left,.modifiers={.control=true}}});
    CHECK(m.gizmo.mode()==CameraGizmoMode::orbit);
    m.send({{.kind=EventKind::key_down,.key=Key::left,.modifiers={.control=true}}});
    CHECK(m.gizmo.walking().active());
    m.frame.keyboard_enabled=false;
    m.send({{.kind=EventKind::key_down,.key=Key::right,.modifiers={.control=true}}});
    CHECK(m.gizmo.mode()==CameraGizmoMode::walk);
}
TEST_CASE("Camera navigation uses available input and cannot change a read-only camera", "[editor][camera_gizmo]") {
    Motion m;m.gizmo.select(CameraGizmoMode::orbit);const auto before=m.frame.pose;
    m.send({{.kind=EventKind::pointer_down,.position={100,100}}},false);CHECK_FALSE(m.gizmo.dragging());
    m.send({{.kind=EventKind::scroll,.position={100,100},.scroll={0,.1F}}},true,false);
    CHECK(m.frame.pose==before);CHECK(m.gizmo.debug_report().situation=="Unavailable");
    m.send({{.kind=EventKind::scroll,.position={100,100},.scroll={0,.1F}}});CHECK(m.frame.pose!=before);
}
TEST_CASE("Camera wheel and zoom child have separate configurable sensitivities", "[editor][camera_gizmo]") {
    Motion normal,fast;fast.frame.drag_speeds.forward=3;
    normal.send({{.kind=EventKind::scroll,.position={100,100},.scroll={0,.1F}}});
    fast.send({{.kind=EventKind::scroll,.position={100,100},.scroll={0,.1F}}});
    CHECK(fast.frame.pose.target.z==Catch::Approx(normal.frame.pose.target.z*3));
    normal.gizmo.select(CameraGizmoMode::zoom);fast.gizmo.select(CameraGizmoMode::zoom);
    fast.frame.drag_speeds.zoom=2;
    const auto previous=normal.frame.pose.target;
    normal.send({{.kind=EventKind::scroll,.position={100,100},.scroll={0,.1F}}});
    fast.send({{.kind=EventKind::scroll,.position={100,100},.scroll={0,.1F}}});
    CHECK(normal.frame.pose.target==previous);CHECK(fast.frame.pose.zoom>normal.frame.pose.zoom);
}
TEST_CASE("Walking pauses for typing and focus loss without losing its mode", "[editor][camera_gizmo]") {
    Motion m;m.gizmo.select(CameraGizmoMode::walk);
    REQUIRE(m.send({{.kind=EventKind::key_down,.key=Key::w}}).changed);
    m.frame.keyboard_enabled=false;CHECK_FALSE(m.send().changed);CHECK(m.gizmo.walking().active());
    m.frame.keyboard_enabled=true;CHECK_FALSE(m.send().changed); // No stuck held key.
    REQUIRE(m.send({{.kind=EventKind::key_down,.key=Key::w}}).changed);
    m.raw.focused=false;
    CHECK_FALSE(m.send({{.kind=EventKind::focus_lost}}).changed);
    CHECK(m.gizmo.walking().active());CHECK(m.gizmo.exclusive());
    m.raw.focused=true;CHECK_FALSE(m.send().changed); // Focus returns without the released key.
    REQUIRE(m.send({{.kind=EventKind::key_down,.key=Key::w}}).changed);
    // A field or flyout that claimed Escape ends held movement, not Walk.
    m.frame.escape_claimed=true;
    CHECK_FALSE(m.send({{.kind=EventKind::key_down,.key=Key::escape}},false).changed);
    CHECK(m.gizmo.walking().active());CHECK_FALSE(m.send().changed);
    // An unclaimed Escape leaves Walk, even when UI used it just to drop button focus.
    m.frame.escape_claimed=false;
    CHECK_FALSE(m.send({{.kind=EventKind::key_down,.key=Key::escape}},false).changed);
    CHECK_FALSE(m.gizmo.walking().active());CHECK_FALSE(m.gizmo.exclusive());
}
TEST_CASE("A chosen camera child survives focus loss and claimed Escape but releases its drag", "[editor][camera_gizmo]") {
    Motion m;m.gizmo.select(CameraGizmoMode::pan);
    m.send({{.kind=EventKind::pointer_down,.position={100,100}}});REQUIRE(m.gizmo.dragging());
    m.raw.focused=false;
    auto reply=m.send({{.kind=EventKind::focus_lost}});
    CHECK(reply.cancelled);CHECK_FALSE(m.gizmo.dragging());
    CHECK(m.gizmo.exclusive());CHECK(m.gizmo.mode()==CameraGizmoMode::pan);
    m.raw.focused=true;
    m.send({{.kind=EventKind::pointer_down,.position={100,100}}});REQUIRE(m.gizmo.dragging());
    m.frame.escape_claimed=true;
    reply=m.send({{.kind=EventKind::key_down,.key=Key::escape}},false);
    CHECK(reply.cancelled);CHECK_FALSE(m.gizmo.dragging());CHECK(m.gizmo.exclusive());
    m.frame.escape_claimed=false;
    // A blocked viewport (dialog, toolbar menu) does not receive the mode change.
    m.send({{.kind=EventKind::key_down,.key=Key::escape}},true,false);CHECK(m.gizmo.exclusive());
    m.send({{.kind=EventKind::key_down,.key=Key::escape}},false);CHECK_FALSE(m.gizmo.exclusive());
    CHECK(m.gizmo.mode()==CameraGizmoMode::orbit);
}
TEST_CASE("Version 8 settings keep their Ctrl-drag optical zoom speed", "[editor][camera_gizmo][settings]") {
    auto v8=decode_settings("vng-editor-settings 8\n60 60 0 10 100\n0.01 10000\n256\n4096\n1\n10000 10 10 10 4\n100\n1 3 0.3 0\n");
    REQUIRE(v8);REQUIRE_FALSE(v8->scroll_moves_camera);
    Motion m;m.frame.drag_speeds=v8->camera_drag;m.frame.move_forward=v8->scroll_moves_camera;
    const auto before=m.frame.pose.zoom;
    m.drag(2,{.control=true}); // 30 logical pixels down.
    // Version 8 applied zoom *= exp(-dy * .01 * its Ctrl-drag multiplier).
    CHECK(m.frame.pose.zoom==Catch::Approx(before*std::exp(-30*.01F*3)));
    CHECK(m.frame.pose.target==Vec3{});
}
TEST_CASE("Camera corner menu is bounded scrollable and retains widgets across mode changes", "[editor][ui][camera_gizmo]") {
    Menu m;m.pump();m.pump();
    const auto b=m.gizmo.bounds();CHECK(b.width<=310.F);CHECK(b.height<=310.F);
    CHECK(b.x>=m.presentation.viewport.x);CHECK(b.y>=m.presentation.viewport.y);
    CHECK(b.x+b.width<=m.presentation.viewport.x+m.presentation.viewport.width);
    auto tree=m.screen.inspect();REQUIRE(tree);const auto count=tree->widgets.size();
    CHECK(std::ranges::any_of(tree->widgets,[](const auto& w){return w.role==ui::WidgetRole::scrollbar&&w.visible;}));
    CHECK_FALSE(m.widget(ui::WidgetRole::slider,"Walk forward").in_layout);
    m.click(m.reveal(ui::WidgetRole::button,"Walk (WASD / Q E)"));m.pump();
    CHECK(m.gizmo.walking().active());
    CHECK(m.widget(ui::WidgetRole::slider,"Walk forward").in_layout);
    CHECK_FALSE(m.widget(ui::WidgetRole::slider,"Orbit degrees/pixel").in_layout);
    REQUIRE(m.screen.inspect());CHECK(m.screen.inspect()->widgets.size()==count);
    m.gizmo.select(CameraGizmoMode::pan);m.pump();
    CHECK_FALSE(m.widget(ui::WidgetRole::slider,"Walk forward").in_layout);
    CHECK(m.widget(ui::WidgetRole::slider,"Pan multiplier").in_layout);
    const auto report=m.gizmo.debug_string();CHECK(report.find("Editor camera")!=std::string::npos);
    m.presentation.target="Entered camera: cockpit";m.pump();
    CHECK(m.gizmo.debug_string().find("Entered camera: cockpit")!=std::string::npos);
}
TEST_CASE("Camera menu heading unfolds the menu without choosing a camera mode", "[editor][ui][camera_gizmo]") {
    Menu m;m.presentation.other_gizmo=true;m.pump();m.pump();
    const auto heading=[&] {
        auto tree=m.screen.inspect();REQUIRE(tree);
        for(const auto& w:tree->widgets)if(w.role==ui::WidgetRole::button&&w.label.starts_with("Camera modes / "))return w;
        FAIL("Missing camera menu heading");return ui::WidgetSnapshot{};
    };
    // An object gizmo is the target: the camera menu stays folded and passive.
    CHECK(heading().label.ends_with(" +"));CHECK_FALSE(m.gizmo.expanded());
    m.click(heading());m.pump();
    CHECK(m.gizmo.expanded());CHECK(heading().label.ends_with(" −"));
    CHECK_FALSE(m.gizmo.exclusive());CHECK_FALSE(m.gizmo.active());
    // Choosing a child is the explicit choice that borrows LMB.
    m.click(m.reveal(ui::WidgetRole::button,"Pan (Shift + MMB)"));
    CHECK(m.gizmo.exclusive());CHECK(m.gizmo.mode()==CameraGizmoMode::pan);
    m.click(m.reveal(ui::WidgetRole::button,"Object tools / selection (Esc)"));
    CHECK_FALSE(m.gizmo.exclusive());CHECK(m.gizmo.expanded());
    // Becoming the camera target again unfolds it; losing that role folds it.
    m.presentation.other_gizmo=false;m.pump();CHECK(m.gizmo.expanded());
    m.presentation.other_gizmo=true;m.pump();CHECK_FALSE(m.gizmo.expanded());
}
TEST_CASE("Walk speed sliders update immediately but persist only on release", "[editor][ui][camera_gizmo]") {
    Menu m;m.gizmo.select(CameraGizmoMode::walk);m.pump();m.pump();
    auto slider=m.reveal(ui::WidgetRole::slider,"Walk forward");
    const Vec2 p{slider.bounds.x+slider.bounds.width*.75F,slider.bounds.y+slider.bounds.height*.5F};
    auto edit=m.pump({{.kind=EventKind::pointer_down,.position=p}});
    CHECK(edit.changed);CHECK_FALSE(edit.committed);CHECK(m.settings.walk.forward>50.F);
    edit=m.pump({{.kind=EventKind::pointer_up,.position=p}});CHECK(edit.committed);
    CHECK(m.settings.walk.sideways==Settings{}.walk.sideways);
    const auto field=m.reveal(ui::WidgetRole::text_field,"Walk forward speed (units/s)");m.click(field);
    edit=m.pump({{.kind=EventKind::key_down,.key=Key::a,.modifiers={.control=true}},
        {.kind=EventKind::text,.text="25"},{.kind=EventKind::key_down,.key=Key::enter}});
    CHECK(edit.changed);CHECK(edit.committed);CHECK(m.settings.walk.forward==25.F);
    const auto before=m.settings;
    edit=m.pump({{.kind=EventKind::key_down,.key=Key::a,.modifiers={.control=true}},
        {.kind=EventKind::text,.text="nan"},{.kind=EventKind::key_down,.key=Key::enter}});
    CHECK_FALSE(edit.changed);CHECK_FALSE(edit.error.empty());CHECK(m.settings==before);
}
TEST_CASE("Camera menu scrolls never leak into optical zoom or camera movement", "[editor][ui][camera_gizmo]") {
    Menu m;m.pump();m.pump();const auto b=m.gizmo.bounds();
    m.pump({{.kind=EventKind::scroll,.position={b.x+20,b.y+80},.scroll={0,-1}}});
    NavigationFrame frame{.pose={0,0,8,{},1},.mode=ViewMode::scene,.viewport=m.presentation.viewport,
        .raw=m.raw,.unhandled=m.unused.events,.drag_speeds={},.walk_speeds={}};
    CHECK_FALSE(m.gizmo.update(frame).changed);
    // Also defend the menu boundary if a host supplied the raw batch as available.
    frame.unhandled=m.raw.events;CHECK_FALSE(m.gizmo.update(frame).changed);
}
