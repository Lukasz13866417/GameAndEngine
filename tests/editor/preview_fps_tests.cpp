#include "../../examples/editor/preview_fps.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
using editor_example::PreviewFps;
using namespace std::chrono_literals;
const PreviewFps::Time start{};
using Activity = PreviewFps::Activity;
vng::editor::preview::FrameInfo frame(vng::u64 generation, vng::u64 id,
                                     std::chrono::nanoseconds duration) {
    vng::editor::preview::FrameInfo info;
    info.generation = generation;
    info.frame_id = id;
    info.interaction.render_started_ns = 1;
    info.interaction.readback_ready_ns = 1 + static_cast<vng::u64>(duration.count());
    return info;
}
}

TEST_CASE("Preview duration appears on the first frame and counts each presented image once", "[editor][fps]") {
    PreviewFps fps;
    fps.update(start, Activity::updating);
    CHECK(fps.preview_label() == "Preview: measuring...");
    fps.presented(frame(1, 1, 10ms), start);
    fps.update(start, Activity::updating);
    CHECK(fps.preview_label() == "Preview: 10.0 ms");
    fps.presented(frame(1, 4, 30ms), start + 40ms); // Two intermediate images were dropped.
    fps.presented(frame(1, 4, 30ms), start + 50ms); // UI redraw, not another worker frame.
    fps.presented(frame(1, 3, 90ms), start + 60ms); // Stale image.
    fps.update(start + 60ms, Activity::updating);
    CHECK(fps.preview_label() == "Preview: 20.0 ms");
    fps.update(start + 500ms, Activity::updating);
    CHECK(fps.preview_label() == "Preview: 20.0 ms");
    CHECK(fps.ui_label() == "UI: 6.0 FPS");
}

TEST_CASE("Preview duration is the arithmetic mean of the latest twenty frame costs", "[editor][fps]") {
    PreviewFps fps;
    for (vng::u64 i = 1; i <= 20; ++i)
        fps.presented(frame(1, i, std::chrono::milliseconds(i)), start);
    fps.update(start, Activity::updating);
    CHECK(fps.preview_label() == "Preview: 10.5 ms");
    fps.presented(frame(1, 21, 41ms), start);
    fps.update(start, Activity::updating);
    CHECK(fps.preview_label() == "Preview: 12.5 ms"); // Replaces 1ms, not 20ms.
    for (vng::u64 i = 22; i <= 41; ++i) fps.presented(frame(1, i, 8ms), start);
    fps.update(start, Activity::updating);
    CHECK(fps.preview_label() == "Preview: 8.0 ms"); // Ring wraps; all earlier costs expire.
}

TEST_CASE("Preview duration survives idle gaps and resumes immediately in short editing bursts", "[editor][fps]") {
    PreviewFps fps;
    fps.presented({}, start);
    fps.update(start, Activity::waiting);
    CHECK(fps.preview_label() == "Preview: waiting");
    fps.presented(frame(1, 1, 10ms), start + 100ms);
    fps.update(start + 100ms, Activity::idle);
    CHECK(fps.preview_label() == "Preview: 10.0 ms (idle)");
    for (int i = 1; i <= 10; ++i) fps.presented(frame(1, 1, 10ms), start + 100ms + i * 50ms);
    fps.update(start + 600ms, Activity::idle);
    CHECK(fps.preview_label() == "Preview: 10.0 ms (idle)");
    CHECK(fps.ui_label() == "UI: 18.3 FPS");
    fps.update(start + 1s, Activity::frozen);
    CHECK(fps.preview_label() == "Preview: 10.0 ms (frozen)");
    fps.update(start + 10s, Activity::updating);
    CHECK(fps.preview_label() == "Preview: 10.0 ms");
    fps.presented(frame(1, 2, 30ms), start + 10030ms);
    fps.update(start + 10030ms, Activity::idle);
    CHECK(fps.preview_label() == "Preview: 20.0 ms (idle)"); // No ten-second idle sample.
}

TEST_CASE("Preview duration resets on worker replacement and rejects absent or invalid timings", "[editor][fps]") {
    PreviewFps fps;
    fps.presented(frame(1, 1, 25ms), start);
    auto missing = frame(2, 1, 10ms);
    missing.interaction = {};
    fps.presented(missing, start + 1s);
    fps.update(start + 1s, Activity::updating);
    CHECK(fps.preview_label() == "Preview: measuring...");
    fps.presented(frame(1, 99, 90ms), start + 1s); // Stale worker.
    fps.presented(frame(2, 2, 0ns), start + 1s); // Empty interval.
    auto reversed = frame(2, 3, 10ms);
    std::swap(reversed.interaction.render_started_ns, reversed.interaction.readback_ready_ns);
    fps.presented(reversed, start + 1s);
    fps.update(start + 1s, Activity::updating);
    CHECK(fps.preview_label() == "Preview: measuring...");
    fps.presented(frame(2, 4, 4500us), start + 1s);
    fps.update(start + 1s, Activity::updating);
    CHECK(fps.preview_label() == "Preview: 4.5 ms");
    fps.update(start + 10s, Activity::updating); // A stalled worker doesn't manufacture samples.
    CHECK(fps.preview_label() == "Preview: 4.5 ms");
}

TEST_CASE("Preview FPS annotation follows the supplied viewport and is clipped to it", "[editor][fps][ui]") {
    auto font = vng::text::Font::load(VNG_TEST_FONT_PATH);
    REQUIRE(font);
    PreviewFps fps;
    for (const vng::ui::Rect viewport : {vng::ui::Rect{20, 100, 900, 600}, {0, 54, 1200, 846}}) {
        vng::ui::DrawList list;
        fps.append(list, *font, viewport);
        REQUIRE(list.commands.size() == 3);
        CHECK(std::get<vng::ui::TextDraw>(list.commands[1]).text == "UI: measuring...");
        CHECK(std::get<vng::ui::TextDraw>(list.commands[2]).text == "Preview: waiting");
        for (const auto index : {1, 2}) {
            const auto& text = std::get<vng::ui::TextDraw>(list.commands[index]);
            CHECK(viewport.contains(text.position));
            CHECK(text.clip.x == viewport.x);
            CHECK(text.clip.y == viewport.y);
            CHECK(text.clip.width == viewport.width);
            CHECK(text.clip.height == viewport.height);
        }
    }
    vng::ui::DrawList list;
    fps.append(list, *font, {});
    CHECK(list.commands.empty());
}
