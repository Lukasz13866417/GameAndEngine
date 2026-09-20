#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <vng/window/glfw.hpp>
#include "../../src/window/glfw_access.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>

TEST_CASE("Native XI2 wheel path preserves legacy scrolling exactly once per event", "[window][x11]") {
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    auto window = vng::window::GlfwWindow::create({.width = 160, .height = 120,
        .title = "VNG isolated native wheel test", .visible = false});
    if (!window || glfwGetPlatform() != GLFW_PLATFORM_X11) {
        std::cerr << "X11 test requires an X11 display\n";
        std::exit(77);
    }
    auto* native = static_cast<GLFWwindow*>(vng::window::detail::GlfwWindowAccess::native_handle(*window));
    auto* display = glfwGetX11Display();
    // Assert the real library enabled the new path, not just the unit-test decoder.
    int count = 0;
    auto* masks = XIGetSelectedEvents(display, glfwGetX11Window(native), &count);
    bool selected = false;
    for (int i = 0; i < count; ++i)
        if (masks[i].deviceid == XIAllMasterDevices && masks[i].mask_len > XI_Motion / 8)
            selected = selected || (XIMaskIsSet(masks[i].mask, XI_Motion) &&
                                    XIMaskIsSet(masks[i].mask, XI_ButtonPress));
    XFree(masks);
    REQUIRE(selected);
    glfwSetWindowPos(native, 10, 10);
    window->show();
    XSync(display, False);
    XTestFakeMotionEvent(display, DefaultScreen(display), 40, 40, CurrentTime);
    XSync(display, False);
    window->poll_events();
    (void)window->take_input();
    // XTEST exposes only traditional wheel buttons, with no smooth axes. They
    // must survive, while core + XI2 deliveries must not duplicate each other.
    for (const int button : {4, 5, 6, 7}) {
        XTestFakeButtonEvent(display, static_cast<unsigned int>(button), True, CurrentTime);
        XTestFakeButtonEvent(display, static_cast<unsigned int>(button), False, CurrentTime);
    }
    XSync(display, False);
    window->poll_events();
    const auto input = window->take_input();
    std::vector<vng::Vec2> deltas;
    for (const auto& event : input.events)
        if (event.kind == vng::input::EventKind::scroll) {
            deltas.push_back(event.scroll);
            CHECK(event.received_ns != 0);
            CHECK(event.position.x == 30.F);
            CHECK(event.position.y == 30.F);
        }
    REQUIRE(deltas == std::vector<vng::Vec2>{{0, 1}, {0, -1}, {1, 0}, {-1, 0}});
    auto moved = std::move(*window);
    XTestFakeButtonEvent(display, 4, True, CurrentTime);
    XTestFakeButtonEvent(display, 4, False, CurrentTime);
    XSync(display, False);
    moved.poll_events();
    const auto after_move = moved.take_input();
    CHECK(std::ranges::count_if(after_move.events, [](const auto& event) {
        return event.kind == vng::input::EventKind::scroll && event.scroll.y == 1;
    }) == 1);
    // Selecting XI2 must not steal GLFW's ordinary clicks, captured drags, or
    // releases. Exercise native events, not direct invocation of callbacks.
    for (const unsigned int button : {1U, 2U, 3U}) {
        XTestFakeMotionEvent(display, DefaultScreen(display), 40, 40, CurrentTime);
        const auto shift = XKeysymToKeycode(display, XK_Shift_L);
        XTestFakeKeyEvent(display, shift, True, CurrentTime);
        XTestFakeButtonEvent(display, button, True, CurrentTime);
        XTestFakeMotionEvent(display, DefaultScreen(display), 220, 190, CurrentTime);
        XTestFakeButtonEvent(display, button, False, CurrentTime);
        XTestFakeKeyEvent(display, shift, False, CurrentTime);
        XSync(display, False);
        moved.poll_events();
        const auto drag = moved.take_input();
        const auto neutral = button == 1 ? 0U : button == 2 ? 2U : 1U;
        CHECK(std::ranges::count_if(drag.events, [neutral](const auto& event) {
            return event.kind == vng::input::EventKind::pointer_down && event.button == neutral &&
                   event.modifiers.shift;
        }) == 1);
        CHECK(std::ranges::count_if(drag.events, [neutral](const auto& event) {
            return event.kind == vng::input::EventKind::pointer_up && event.button == neutral;
        }) == 1);
        CHECK(std::ranges::any_of(drag.events, [](const auto& event) {
            return event.kind == vng::input::EventKind::pointer_move &&
                   event.position == vng::Vec2{210, 180};
        }));
    }
}
