#include <vng/render/graphics_state.hpp>
#include <vng/render/program.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <type_traits>

namespace {
struct SpecializedSetting { int value; };

struct PortableBackend {
    bool& enabled;
    std::expected<void, int> set(vng::render::DepthTest test)
    {
        enabled = test.enabled;
        return {};
    }
};

struct SpecializedBackend : PortableBackend {
    using PortableBackend::set;
    std::expected<void, int> set(SpecializedSetting value)
    {
        if (value.value < 0) return std::unexpected(42);
        return {};
    }
};
} // namespace

TEST_CASE("Graphics state facade infers backend capabilities and forwards errors",
          "[render][graphics-state]")
{
    using Portable = vng::render::GraphicsState<PortableBackend>;
    using Specialized = vng::render::GraphicsState<SpecializedBackend>;
    STATIC_CHECK(vng::render::SupportsGraphicsSetting<Portable, vng::render::DepthTest>);
    STATIC_CHECK_FALSE(vng::render::SupportsGraphicsSetting<Portable, SpecializedSetting>);
    STATIC_CHECK(vng::render::SupportsGraphicsSetting<Specialized, SpecializedSetting>);
    STATIC_CHECK_FALSE(vng::render::SupportsGraphicsSetting<Specialized, int>);
    STATIC_CHECK_FALSE(vng::render::SupportsGraphicsSetting<Specialized, bool>);
    STATIC_CHECK(sizeof(Portable) == sizeof(PortableBackend));

    bool enabled = false;
    auto graphics = vng::render::GraphicsState{SpecializedBackend{{enabled}}};
    REQUIRE(graphics.set(vng::render::DepthTest{true}));
    CHECK(enabled);
    REQUIRE(graphics.set(vng::render::DepthTest{false}));
    CHECK_FALSE(enabled);
    auto unsupported = graphics.set(SpecializedSetting{-1});
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error() == 42);
}

namespace fake_backend {
struct Device {};
struct Program {};
inline std::expected<Program, int> compile_graphics_program(
    const Device&, const vng::shader::GraphicsProgram&)
{
    return Program{};
}
} // namespace fake_backend

static_assert(std::same_as<
    decltype(vng::render::compile_program(
        std::declval<const fake_backend::Device&>(),
        std::declval<const vng::shader::GraphicsProgram&>())),
    std::expected<fake_backend::Program, int>>);
