#include <vng/opengl/opengl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string>

#include "../support/glfw_opengl.hpp"

namespace {

[[noreturn]] void skip_ctest(const std::string& reason)
{
    std::cerr << "OpenGL diagnostic test skipped: " << reason << '\n';
    std::exit(77);
}

[[nodiscard]] bool contains(
    const std::vector<vng::opengl::DebugMessage>& messages,
    const std::string& text)
{
    return std::ranges::any_of(messages, [&](const auto& message) {
        return message.message.find(text) != std::string::npos;
    });
}

[[nodiscard]] bool contains(
    const std::vector<vng::opengl::Diagnostic>& diagnostics,
    const std::string& text)
{
    return std::ranges::any_of(diagnostics, [&](const auto& diagnostic) {
        return diagnostic.message.find(text) != std::string::npos;
    });
}

} // namespace

TEST_CASE("diagnostic cursors retain new debug messages independently of take",
          "[opengl][diagnostics]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng diagnostic cursor debug test");
    if (!window) {
        skip_ctest(window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest(access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);

    (void)device->take_debug_messages();
    REQUIRE(device->debug_marker("diagnostic before cursor"));
    auto cursor = device->diagnostic_cursor();
    REQUIRE(cursor);
    REQUIRE(device->debug_marker("diagnostic after cursor"));

    auto first = device->diagnostics_since(*cursor);
    REQUIRE(first);
    CHECK_FALSE(contains(first->debug_messages, "diagnostic before cursor"));
    CHECK(contains(first->debug_messages, "diagnostic after cursor"));
    CHECK(first->lifecycle_diagnostics.empty());

    const auto consumed = device->take_debug_messages();
    CHECK(contains(consumed, "diagnostic before cursor"));
    CHECK(contains(consumed, "diagnostic after cursor"));
    CHECK(device->take_debug_messages().empty());

    // Snapshot history is a separate consumer; taking messages does not erase
    // the evidence visible from an existing cursor.
    auto after_take = device->diagnostics_since(*cursor);
    REQUIRE(after_take);
    CHECK(contains(after_take->debug_messages, "diagnostic after cursor"));

    REQUIRE(device->debug_marker("diagnostic still later"));
    auto with_later_message = device->diagnostics_since(*cursor);
    REQUIRE(with_later_message);
    CHECK(contains(
        with_later_message->debug_messages,
        "diagnostic still later"));
}

TEST_CASE("diagnostic cursor retains lifecycle history independently of take",
          "[opengl][diagnostics][lifecycle]")
{
    auto window = vng::test::create_hidden_opengl_window(
        8, 8, "vng diagnostic cursor lifecycle test");
    if (!window) {
        skip_ctest(window.error().message);
    }
    auto access = window->make_current();
    if (!access) {
        skip_ctest(access.error().message);
    }
    auto device = vng::opengl::Device::create(*access);
    REQUIRE(device);

    (void)device->take_lifecycle_diagnostics();
    auto cursor = device->diagnostic_cursor();
    REQUIRE(cursor);
    {
        auto state = vng::opengl::RenderStateScope::capture(*device);
        REQUIRE(state);
        window->release_current();
        // Destruction without the context records a lifecycle failure. State
        // was only queried, not changed, so omitting restoration is harmless.
    }
    REQUIRE(window->make_current());

    const auto consumed = device->take_lifecycle_diagnostics();
    CHECK(contains(consumed, "destroyed without its owning context current"));
    CHECK(device->take_lifecycle_diagnostics().empty());

    auto after_take = device->diagnostics_since(*cursor);
    REQUIRE(after_take);
    CHECK(contains(
        after_take->lifecycle_diagnostics,
        "destroyed without its owning context current"));
}

TEST_CASE("diagnostic cursors reject a different OpenGL context",
          "[opengl][diagnostics][context]")
{
    auto first_window = vng::test::create_hidden_opengl_window(
        8, 8, "vng first diagnostic context");
    if (!first_window) {
        skip_ctest(first_window.error().message);
    }
    auto first_access = first_window->make_current();
    REQUIRE(first_access);
    auto first_device = vng::opengl::Device::create(*first_access);
    REQUIRE(first_device);
    auto cursor = first_device->diagnostic_cursor();
    REQUIRE(cursor);

    auto second_window = vng::test::create_hidden_opengl_window(
        8, 8, "vng second diagnostic context");
    REQUIRE(second_window);
    auto second_access = second_window->make_current();
    REQUIRE(second_access);
    auto second_device = vng::opengl::Device::create(*second_access);
    REQUIRE(second_device);

    auto wrong_context = second_device->diagnostics_since(*cursor);
    REQUIRE_FALSE(wrong_context);
    CHECK(wrong_context.error().code
          == vng::opengl::ErrorCode::incompatible_device);

    REQUIRE(first_window->make_current());
    CHECK(first_device->diagnostics_since(*cursor).has_value());
}
