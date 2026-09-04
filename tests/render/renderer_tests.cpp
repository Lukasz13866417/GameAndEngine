#include <array>
#include <expected>
#include <span>
#include <string>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include <vng/render/renderer.hpp>
#include <vng/render/frame.hpp>

namespace fake_render_backend {

struct Ticket final {
    int value{};
};

struct Device final {};

struct OtherFrame final {};

struct Diagnostic final {
    std::string message;
};

struct Frame final {
    std::size_t draws{};
    int total{};
    vng::Extent2D target_extent{};
    bool open{true};

    [[nodiscard]] bool active() const noexcept { return open; }
    [[nodiscard]] vng::Extent2D extent() const noexcept { return target_extent; }
    void end() noexcept { open = false; }
};

struct CaptureRequest final {
    int scale{1};
};

[[nodiscard]] std::expected<Frame, Diagnostic> begin_backend_frame(
    Device&,
    vng::render::DefaultTarget,
    const vng::render::FrameDesc& description)
{
    if (description.extent.empty()) {
        return std::unexpected(Diagnostic{"empty fake frame"});
    }
    Frame result;
    result.target_extent = description.extent;
    return result;
}

class TestRenderer final : public vng::render::Renderer<Ticket> {
public:
    [[nodiscard]] std::expected<void, Diagnostic> render(
        Frame& frame,
        const vng::render::RenderView&,
        std::span<const Ticket> tickets)
    {
        frame.draws += tickets.size();
        for (const auto ticket : tickets) {
            frame.total += ticket.value;
        }
        return {};
    }

    [[nodiscard]] std::expected<int, Diagnostic> capture(
        Frame&,
        const vng::render::RenderView&,
        std::span<const Ticket> tickets,
        const CaptureRequest& request)
    {
        int result = 0;
        for (const auto ticket : tickets) {
            result += ticket.value * request.scale;
        }
        return result;
    }

    [[nodiscard]] std::expected<int, Diagnostic> diagnose(
        Frame&,
        const vng::render::RenderView&,
        std::span<const Ticket> tickets,
        const CaptureRequest& request)
    {
        return static_cast<int>(tickets.size()) * request.scale;
    }
};

class MissingRender final : public vng::render::Renderer<Ticket> {};

class StatefulRenderer final : public vng::render::Renderer<Ticket> {
public:
    int state{};

    void render(
        Frame&,
        const vng::render::RenderView&,
        std::span<const Ticket>)
    {}
};

// Merely imitating the spelling of the API does not opt a type into renderer
// orchestration. The base class is the explicit, zero-cost declaration of
// intent.
class RendererLookalike final {
public:
    using ticket_type = Ticket;

    void render(
        Frame&,
        const vng::render::RenderView&,
        std::span<const Ticket>)
    {}
};

} // namespace fake_render_backend

TEST_CASE("a concrete renderer owns its policy and accepts ticket batches")
{
    using namespace fake_render_backend;

    TestRenderer renderer;

    STATIC_CHECK(vng::render::RendererType<TestRenderer>);
    STATIC_CHECK(vng::render::RendererFor<TestRenderer, Frame>);
    STATIC_CHECK(std::is_same_v<
        vng::render::renderer_ticket_t<TestRenderer>, Ticket>);

    Frame frame;
    const auto view = vng::render::RenderView::without_camera({320, 200});
    const std::array tickets{Ticket{2}, Ticket{5}};
    REQUIRE(renderer.render(frame, view, std::span{tickets}).has_value());
    CHECK(frame.draws == 2);
    CHECK(frame.total == 7);

    auto capture = renderer.capture(
        frame, view, std::span{tickets}.subspan(1, 1), CaptureRequest{3});
    REQUIRE(capture.has_value());
    CHECK(*capture == 15);

    auto diagnosis = renderer.diagnose(
        frame, view, std::span{tickets}, CaptureRequest{4});
    REQUIRE(diagnosis.has_value());
    CHECK(*diagnosis == 8);
}

TEST_CASE("render_one adapts one ticket without constraining renderer results")
{
    using namespace fake_render_backend;

    TestRenderer renderer;
    Frame frame;
    const auto view = vng::render::RenderView::without_camera({320, 200});

    REQUIRE(vng::render::render_one(
        renderer, frame, view, Ticket{11}).has_value());
    CHECK(frame.draws == 1);
    CHECK(frame.total == 11);
}

TEST_CASE("renderer concepts reject missing policy and mismatched backends")
{
    using namespace fake_render_backend;

    STATIC_CHECK_FALSE(vng::render::RendererType<int>);
    STATIC_CHECK_FALSE(
        vng::render::RendererType<vng::render::Renderer<Ticket>>);
    STATIC_CHECK_FALSE(vng::render::RendererType<RendererLookalike>);
    STATIC_CHECK(vng::render::RendererType<MissingRender>);
    STATIC_CHECK_FALSE(vng::render::RendererFor<MissingRender, Frame>);
    STATIC_CHECK_FALSE(vng::render::RendererFor<TestRenderer, OtherFrame>);
}

TEST_CASE("renderer identity has no runtime dispatch or storage overhead")
{
    using namespace fake_render_backend;

    STATIC_CHECK(std::is_empty_v<vng::render::Renderer<Ticket>>);
    STATIC_CHECK_FALSE(std::is_polymorphic_v<vng::render::Renderer<Ticket>>);
    STATIC_CHECK_FALSE(
        std::has_virtual_destructor_v<vng::render::Renderer<Ticket>>);
    STATIC_CHECK(std::is_empty_v<TestRenderer>);
    STATIC_CHECK_FALSE(std::is_polymorphic_v<StatefulRenderer>);
    STATIC_CHECK(vng::render::RendererFor<StatefulRenderer, Frame>);
    STATIC_CHECK(sizeof(StatefulRenderer) == sizeof(int));
}

TEST_CASE("frame creation dispatches to a backend without entering render core")
{
    STATIC_CHECK(
        vng::render::FrameDesc{}.color_encoding
        == vng::render::ColorEncoding::srgb);

    fake_render_backend::Device device;
    auto frame = vng::render::begin_frame(
        device,
        vng::render::FrameDesc{
            .extent = {640, 360},
            .clear_color = std::array<vng::f32, 4>{0.1F, 0.2F, 0.3F, 1.0F},
            .clear_depth = 1.0F,
        });
    REQUIRE(frame);
    STATIC_CHECK(vng::render::Frame<fake_render_backend::Frame>);
    CHECK(frame->active());
    CHECK(frame->extent() == vng::Extent2D{640, 360});
    frame->end();
    CHECK_FALSE(frame->active());

    auto invalid = vng::render::begin_frame(
        device, vng::render::FrameDesc{});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().message == "empty fake frame");
}
