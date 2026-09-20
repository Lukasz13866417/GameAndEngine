#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <vng/render/program.hpp>
#include <vng/render/graphics_state.hpp>

namespace fake_backend {

struct Device final {};
struct BackendProgram final {};
struct UnsupportedSource final {};

[[nodiscard]] BackendProgram compile_graphics_program(
    const Device&, const vng::shader::GraphicsProgram&)
{
    return {};
}

} // namespace fake_backend

template<class Device, class Source>
concept CanCompileProgram = requires(const Device& device, const Source& source) {
    vng::render::compile_program(device, source);
};

TEST_CASE("program compilation selects a backend without coupling render core to it")
{
    using Result = decltype(vng::render::compile_program(
        std::declval<const fake_backend::Device&>(),
        std::declval<const vng::shader::GraphicsProgram&>()));

    STATIC_CHECK(std::is_same_v<Result, fake_backend::BackendProgram>);
    STATIC_CHECK(CanCompileProgram<
        fake_backend::Device, vng::shader::GraphicsProgram>);
    STATIC_CHECK_FALSE(CanCompileProgram<
        fake_backend::Device, fake_backend::BackendProgram>);
    STATIC_CHECK_FALSE(CanCompileProgram<
        fake_backend::Device, fake_backend::UnsupportedSource>);
}

TEST_CASE("portable depth settings have conservative comparable defaults")
{
    constexpr vng::render::DepthState defaults{};
    STATIC_CHECK(!defaults.test);
    STATIC_CHECK(!defaults.write);
    STATIC_CHECK(defaults.compare == vng::render::DepthCompare::less);
    STATIC_CHECK(std::is_trivially_copyable_v<decltype(defaults)>);

    constexpr vng::render::DepthState custom{
        .test = true, .write = false,
        .compare = vng::render::DepthCompare::greater_equal,
    };
    constexpr auto same = custom;
    STATIC_CHECK(custom == same);
    STATIC_CHECK(custom != defaults);
}
