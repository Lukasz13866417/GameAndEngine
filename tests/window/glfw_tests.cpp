#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <vng/window/glfw.hpp>
#include "../../src/window/glfw_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

template<class T>
concept HasGraphicsContext = requires(T& value) {
    value.make_current();
    value.present();
};

static_assert(vng::window::Window<vng::window::GlfwWindow>);
static_assert(!HasGraphicsContext<vng::window::GlfwWindow>);

[[noreturn]] void skip_ctest(const std::string& reason)
{
    std::cerr << "GLFW test skipped: " << reason << '\n';
    std::exit(77);
}

[[nodiscard]] vng::window::WindowDesc hidden_window_description()
{
    return {
        .width = 16,
        .height = 16,
        .title = "vng hidden GLFW event test",
        .visible = false,
        .resizable = false,
    };
}

} // namespace

TEST_CASE("GLFW delivers surface and whole mesh mode keys through neutral input", "[window][glfw][input]") {
    auto window=vng::window::GlfwWindow::create(hidden_window_description());
    if(!window)skip_ctest(window.error().message);
    auto* native=static_cast<GLFWwindow*>(vng::window::detail::GlfwWindowAccess::native_handle(*window));
    auto key=glfwSetKeyCallback(native,nullptr);REQUIRE(key);glfwSetKeyCallback(native,key);
    (void)window->take_input();
    for(const auto& [physical,logical]:{std::pair{GLFW_KEY_4,vng::input::Key::four},
        std::pair{GLFW_KEY_5,vng::input::Key::five}}) {
    key(native,physical,0,GLFW_PRESS,0);
    key(native,physical,0,GLFW_RELEASE,0);
    const auto input=window->take_input();
    REQUIRE(input.events.size()==2);
    CHECK(input.events[0].kind==vng::input::EventKind::key_down);
    CHECK(input.events[1].kind==vng::input::EventKind::key_up);
    CHECK(input.events[0].key==logical);
    CHECK(input.events[1].key==logical);
    }
}

TEST_CASE("Both physical Control keys produce the same arrow shortcut modifiers", "[window][glfw][input]") {
    auto window=vng::window::GlfwWindow::create(hidden_window_description());
    if(!window)skip_ctest(window.error().message);
    auto* native=static_cast<GLFWwindow*>(vng::window::detail::GlfwWindowAccess::native_handle(*window));
    auto key=glfwSetKeyCallback(native,nullptr);REQUIRE(key);glfwSetKeyCallback(native,key);
    for(int control:{GLFW_KEY_LEFT_CONTROL,GLFW_KEY_RIGHT_CONTROL}) {
        (void)window->take_input();
        key(native,control,0,GLFW_PRESS,GLFW_MOD_CONTROL);
        key(native,GLFW_KEY_RIGHT,0,GLFW_PRESS,GLFW_MOD_CONTROL);
        key(native,GLFW_KEY_RIGHT,0,GLFW_RELEASE,GLFW_MOD_CONTROL);
        key(native,control,0,GLFW_RELEASE,0);
        const auto input=window->take_input();
        int arrows{};
        for(const auto& event:input.events)if(event.key==vng::input::Key::right) {
            CHECK(event.modifiers.control);++arrows;
        }
        CHECK(arrows==2);
    }
}

TEST_CASE("Window modes retain decorations and windowed bounds across fullscreen and moves",
          "[window][glfw][modes]") {
    auto description = hidden_window_description();
    description.width=640; description.height=480; description.resizable=true;
    auto window = vng::window::GlfwWindow::create(description);
    if (!window) skip_ctest(window.error().message);
    auto* native=static_cast<GLFWwindow*>(vng::window::detail::GlfwWindowAccess::native_handle(*window));
    CHECK(glfwGetWindowAttrib(native,GLFW_DECORATED)==GLFW_TRUE);
    CHECK_FALSE(window->fullscreen());
    auto key=glfwSetKeyCallback(native,nullptr); REQUIRE(key); glfwSetKeyCallback(native,key);
    (void)window->take_input();
    key(native,GLFW_KEY_F11,0,GLFW_PRESS,0);
    const auto events=window->take_input();
    REQUIRE(events.events.size()==1);
    CHECK(events.events[0].key==vng::input::Key::f11);
    std::optional<vng::window::Diagnostic> error;
    std::thread other([&] {
        auto result=window->set_fullscreen(true);
        if(!result) error=result.error();
        window->maximize(); window->restore();
    }); other.join();
    REQUIRE(error); CHECK(error->code==vng::window::ErrorCode::wrong_thread);
    REQUIRE(window->set_fullscreen(true));
    CHECK(window->fullscreen());
    REQUIRE(window->set_fullscreen(true)); // Idempotent: preserve restoration bounds.
    auto moved=std::move(*window);
    CHECK_FALSE(window->set_fullscreen(false));
    window->maximize(); window->restore(); CHECK_FALSE(window->maximized()); CHECK_FALSE(window->fullscreen());
    CHECK(moved.fullscreen());
    REQUIRE(moved.set_fullscreen(false));
    CHECK_FALSE(moved.fullscreen());
    moved.hide();
    CHECK(glfwGetWindowAttrib(native,GLFW_DECORATED)==GLFW_TRUE);
    int width{},height{}; glfwGetWindowSize(native,&width,&height);
    CHECK(width==640); CHECK(height==480);
}

TEST_CASE("Maximized window hint and restoration use the same decorated native window",
          "[window][glfw][modes]") {
    auto description=hidden_window_description();
    description.width=640; description.height=480; description.resizable=true; description.maximized=true;
    auto window=vng::window::GlfwWindow::create(description);
    if(!window) skip_ctest(window.error().message);
    auto* native=static_cast<GLFWwindow*>(vng::window::detail::GlfwWindowAccess::native_handle(*window));
    // No window-manager dependency for the hidden creation hint.
    CHECK(window->maximized());
    CHECK_FALSE(window->fullscreen());
    window->show();
    const auto wait_for = [&](auto predicate) {
        for(int i=0;i<100 && !predicate();++i) {
            window->poll_events();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return predicate();
    };
    REQUIRE(wait_for([&] {return window->maximized();}));
    REQUIRE(window->set_fullscreen(true));
    CHECK(window->fullscreen());
    REQUIRE(window->set_fullscreen(false));
    REQUIRE(wait_for([&] {return window->maximized();}));
    CHECK(glfwGetWindowAttrib(native,GLFW_DECORATED)==GLFW_TRUE);
    window->restore();
    REQUIRE(wait_for([&] {return !window->maximized();}));
    window->maximize();
    REQUIRE(wait_for([&] {return window->maximized();}));
    window->hide();
}


TEST_CASE("GLFW queues neutral UI input per window across moves", "[window][input]") {
    auto a = vng::window::GlfwWindow::create(hidden_window_description());
    if (!a) skip_ctest(a.error().message);
    auto b = vng::window::GlfwWindow::create(hidden_window_description()); REQUIRE(b);
    auto* native = static_cast<GLFWwindow*>(vng::window::detail::GlfwWindowAccess::native_handle(*a));
    (void)a->take_input(); (void)b->take_input();
    auto key = glfwSetKeyCallback(native,nullptr); REQUIRE(key); glfwSetKeyCallback(native,key);
    auto character = glfwSetCharCallback(native,nullptr); REQUIRE(character); glfwSetCharCallback(native,character);
    auto mouse = glfwSetMouseButtonCallback(native,nullptr); REQUIRE(mouse); glfwSetMouseButtonCallback(native,mouse);
    key(native,GLFW_KEY_A,0,GLFW_PRESS,GLFW_MOD_CONTROL);
    character(native,0x00e9);
    mouse(native,GLFW_MOUSE_BUTTON_LEFT,GLFW_PRESS,0);
    auto moved = std::move(*a);
    key(native,GLFW_KEY_A,0,GLFW_RELEASE,GLFW_MOD_CONTROL);
    auto events = moved.take_input(); REQUIRE(events.events.size() == 4);
    CHECK(events.events[0].key == vng::input::Key::a);
    CHECK(events.events[0].modifiers.control);
    CHECK(events.events[1].text == "é");
    CHECK(events.events[2].kind == vng::input::EventKind::pointer_down);
    CHECK(events.events[3].kind == vng::input::EventKind::key_up);
    CHECK(moved.take_input().events.empty()); CHECK(b->take_input().events.empty());
    CHECK(a->take_input().framebuffer.empty());
}

TEST_CASE("GLFW forwards middle button modifiers and wheel for viewport navigation", "[window][input]") {
    auto window = vng::window::GlfwWindow::create(hidden_window_description());
    if (!window) skip_ctest(window.error().message);
    auto* native = static_cast<GLFWwindow*>(vng::window::detail::GlfwWindowAccess::native_handle(*window));
    (void)window->take_input();
    auto mouse = glfwSetMouseButtonCallback(native, nullptr);
    auto scroll = glfwSetScrollCallback(native, nullptr);
    REQUIRE(mouse);
    REQUIRE(scroll);
    glfwSetMouseButtonCallback(native, mouse);
    glfwSetScrollCallback(native, scroll);
    mouse(native, GLFW_MOUSE_BUTTON_MIDDLE, GLFW_PRESS, GLFW_MOD_SHIFT);
    mouse(native, GLFW_MOUSE_BUTTON_MIDDLE, GLFW_RELEASE, GLFW_MOD_SHIFT);
    mouse(native, GLFW_MOUSE_BUTTON_MIDDLE, GLFW_PRESS, GLFW_MOD_CONTROL);
    mouse(native, GLFW_MOUSE_BUTTON_MIDDLE, GLFW_RELEASE, GLFW_MOD_CONTROL);
    scroll(native, 0, 2.5);
    const auto input = window->take_input();
    REQUIRE(input.events.size() == 5);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(input.events[i].button == 2);
        CHECK(input.events[i].kind == (i % 2 == 0 ? vng::input::EventKind::pointer_down
                                                 : vng::input::EventKind::pointer_up));
        CHECK(input.events[i].modifiers.shift == (i < 2));
        CHECK(input.events[i].modifiers.control == (i >= 2));
    }
    CHECK(input.events[4].kind == vng::input::EventKind::scroll);
    CHECK(input.events[4].scroll == vng::Vec2{0, 2.5F});
    scroll(native, 0, 1.0 / 120.0);
    scroll(native, 0, -.0001);
    const auto fractional = window->take_input();
    REQUIRE(fractional.events.size() == 2);
    CHECK(fractional.events[0].scroll.y == 1.F / 120.F);
    CHECK(fractional.events[1].scroll.y == -.0001F);
    CHECK(fractional.events[0].received_ns != 0);
}

TEST_CASE("GLFW validates neutral dimensions before native narrowing",
          "[window][glfw][validation]")
{
    auto zero_width = vng::window::GlfwWindow::create(
        vng::window::WindowDesc{.width = 0});
    REQUIRE_FALSE(zero_width);
    CHECK(zero_width.error().code
          == vng::window::ErrorCode::window_creation_failed);

    if constexpr (std::numeric_limits<std::uint32_t>::max()
                  > static_cast<std::uint32_t>(
                      std::numeric_limits<int>::max())) {
        auto too_wide = vng::window::GlfwWindow::create(
            vng::window::WindowDesc{
                .width = std::numeric_limits<std::uint32_t>::max(),
            });
        REQUIRE_FALSE(too_wide);
        CHECK(too_wide.error().code
              == vng::window::ErrorCode::window_creation_failed);
    }
}

TEST_CASE("Window visibility and close cancellation preserve the native lifetime", "[window][glfw][visibility]") {
    auto window = vng::window::GlfwWindow::create(hidden_window_description());
    if (!window) skip_ctest(window.error().message);
    CHECK_FALSE(window->visible());
    window->show();
    CHECK(window->visible());
    window->request_close();
    CHECK(window->should_close());
    window->cancel_close();
    CHECK_FALSE(window->should_close());
    window->hide();
    CHECK_FALSE(window->visible());
    CHECK(window->valid());
    auto moved = std::move(*window);
    window->show(); window->hide(); window->cancel_close();
    CHECK_FALSE(window->visible());
    CHECK(window->should_close());
    moved.show();
    CHECK(moved.visible());
    moved.hide();
}

TEST_CASE("GLFW instance event calls require a live receiver across moves",
          "[window][glfw][events]")
{
    auto source = vng::window::GlfwWindow::create(hidden_window_description());
    if (!source) {
        skip_ctest("GLFW window unavailable: " + source.error().message);
    }
    auto second = vng::window::GlfwWindow::create(hidden_window_description());
    REQUIRE(second.has_value());

    source->poll_events();
    auto moved = std::move(*source);
    CHECK_FALSE(source->valid());
    CHECK(moved.valid());
    CHECK(source->should_close());
    CHECK_FALSE(moved.should_close());
    CHECK_FALSE(source->key_down(vng::window::Key::space));
    CHECK_FALSE(moved.key_down(static_cast<vng::window::Key>(-1)));
    CHECK_FALSE(moved.key_down(vng::window::Key::escape));
    CHECK_FALSE(moved.key_down(vng::window::Key::w));
    CHECK_FALSE(source->key_down(vng::window::Key::w));

    // The destination remains the live receiver. Calling a moved-from object
    // is harmless, but it no longer forwards to GLFW's process-wide queue.
    moved.poll_events();
    source->poll_events();

    // Wake the process-wide wait from a worker. A small wake-count ceiling
    // prevents a moved-from implementation that merely no-ops from turning
    // this into a busy wait while avoiding an unbounded test hang.
    std::atomic<bool> posted{false};
    std::thread notifier([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        posted.store(true, std::memory_order_release);
        glfwPostEmptyEvent();
    });

    std::size_t wake_count = 0;
    while (!posted.load(std::memory_order_acquire) && wake_count < 32) {
        moved.wait_events();
        ++wake_count;
    }
    notifier.join();

    CHECK(posted.load(std::memory_order_acquire));
    CHECK(wake_count < 32);
    second->poll_events();
}

TEST_CASE("one live GLFW lifetime has one initialization and event thread",
          "[window][glfw][events][thread]")
{
    auto owner = vng::window::GlfwWindow::create(hidden_window_description());
    if (!owner) {
        skip_ctest("GLFW window unavailable: " + owner.error().message);
    }

    bool worker_created_window = false;
    bool worker_key_down = true;
    std::optional<vng::window::Diagnostic> worker_diagnostic;
    std::thread worker([&] {
        worker_key_down = owner->key_down(vng::window::Key::r);
        auto result = vng::window::GlfwWindow::create(
            hidden_window_description());
        worker_created_window = result.has_value();
        if (!result) {
            worker_diagnostic.emplace(std::move(result.error()));
        }
    });
    worker.join();

    CHECK_FALSE(worker_created_window);
    CHECK_FALSE(worker_key_down);
    REQUIRE(worker_diagnostic.has_value());
    CHECK(worker_diagnostic->code == vng::window::ErrorCode::wrong_thread);

    // A rejected cross-thread attempt must not disturb the owning thread.
    owner->poll_events();
    auto another = vng::window::GlfwWindow::create(hidden_window_description());
    REQUIRE(another.has_value());
    another->poll_events();
}
