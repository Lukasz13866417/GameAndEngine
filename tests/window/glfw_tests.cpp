#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <vng/window/glfw.hpp>

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
    std::optional<vng::window::Diagnostic> worker_diagnostic;
    std::thread worker([&] {
        auto result = vng::window::GlfwWindow::create(
            hidden_window_description());
        worker_created_window = result.has_value();
        if (!result) {
            worker_diagnostic.emplace(std::move(result.error()));
        }
    });
    worker.join();

    CHECK_FALSE(worker_created_window);
    REQUIRE(worker_diagnostic.has_value());
    CHECK(worker_diagnostic->code == vng::window::ErrorCode::wrong_thread);

    // A rejected cross-thread attempt must not disturb the owning thread.
    owner->poll_events();
    auto another = vng::window::GlfwWindow::create(hidden_window_description());
    REQUIRE(another.has_value());
    another->poll_events();
}
