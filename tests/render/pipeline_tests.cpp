#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <vng/render/pipeline.hpp>

namespace fake_backend {

struct Device final {};
struct BackendProgram final {};

struct Pipeline final {
    vng::render::GraphicsPipelineDesc description;
};

[[nodiscard]] constexpr Pipeline compile_graphics_pipeline(
    const Device&,
    const vng::shader::GraphicsProgram&,
    vng::render::GraphicsPipelineDesc description)
{
    return {.description = description};
}

[[nodiscard]] constexpr Pipeline realize_graphics_pipeline(
    const Device&,
    BackendProgram,
    vng::render::GraphicsPipelineDesc description)
{
    return {.description = description};
}

} // namespace fake_backend

template<class Device, class Program>
concept CanCompilePipeline = requires(
    const Device& device,
    Program&& program) {
    vng::render::compile_pipeline(
        device, std::forward<Program>(program));
};

TEST_CASE("graphics pipeline descriptions have portable conservative defaults")
{
    constexpr vng::render::GraphicsPipelineDesc pipeline{};

    STATIC_CHECK(!pipeline.depth.test);
    STATIC_CHECK(!pipeline.depth.write);
    STATIC_CHECK(
        pipeline.depth.compare == vng::render::DepthCompare::less);
    STATIC_CHECK(pipeline.cull == vng::render::CullMode::none);
    STATIC_CHECK(
        pipeline.front_face == vng::render::FrontFace::counter_clockwise);
    STATIC_CHECK(
        pipeline.output_encoding == vng::render::ColorEncoding::srgb);

    STATIC_CHECK(std::is_trivially_copyable_v<decltype(pipeline)>);
}

TEST_CASE("graphics pipeline descriptions are aggregate-configurable and comparable")
{
    constexpr vng::render::GraphicsPipelineDesc first{
        .depth = {
            .test = true,
            .write = false,
            .compare = vng::render::DepthCompare::greater_equal,
        },
        .cull = vng::render::CullMode::back,
        .front_face = vng::render::FrontFace::clockwise,
        .output_encoding = vng::render::ColorEncoding::srgb,
    };
    constexpr auto same = first;
    constexpr auto different = vng::render::GraphicsPipelineDesc{};

    STATIC_CHECK(first == same);
    STATIC_CHECK(first != different);
}

TEST_CASE("pipeline compilation selects a backend without coupling render core to it")
{
    using Result = decltype(vng::render::compile_pipeline(
        std::declval<const fake_backend::Device&>(),
        std::declval<const vng::shader::GraphicsProgram&>(),
        std::declval<vng::render::GraphicsPipelineDesc>()));

    STATIC_CHECK(std::is_same_v<Result, fake_backend::Pipeline>);
    STATIC_CHECK_FALSE(CanCompilePipeline<
        fake_backend::Device, fake_backend::BackendProgram>);
}

TEST_CASE("backend program realization is an explicit expert operation")
{
    constexpr vng::render::GraphicsPipelineDesc description{
        .cull = vng::render::CullMode::back,
    };
    constexpr fake_backend::Device device;

    constexpr auto pipeline = vng::render::expert::realize_pipeline(
        device, fake_backend::BackendProgram{}, description);

    STATIC_CHECK(std::is_same_v<
        std::remove_cv_t<decltype(pipeline)>, fake_backend::Pipeline>);
    STATIC_CHECK(pipeline.description == description);
}
