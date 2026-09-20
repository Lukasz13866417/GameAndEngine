#include "../../examples/editor/preview_viewport.hpp"
#include "../../examples/editor/preview_mailbox.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>
namespace {
using namespace vng;
using namespace editor_example;
} // namespace
TEST_CASE("A paused preview retains an image arriving before its scene acknowledgement",
          "[editor][preview][apply]") {
    PreviewMailbox mailbox;
    editor::preview::PreviewFrame image{{1, 1, 7, 38, 12, 3}, std::vector<std::byte>(4, std::byte{42})};
    const auto* allocation = image.rgba.data();
    mailbox.offer(std::move(image), 7);
    CHECK_FALSE(mailbox.take(7, 37, 37, 37));
    CHECK_FALSE(mailbox.take(7, 37, 37, 37)); // No second frame is published.
    auto after_ack = mailbox.take(7, 38, 37, 38);
    REQUIRE(after_ack);
    CHECK(after_ack->info.scene_revision == 38);
    CHECK(after_ack->rgba.data() == allocation); // No extra pixel copy.
    CHECK_FALSE(mailbox.take(7, 38, 38, 38));
}
TEST_CASE("Completed preview mailbox is bounded and respects generation and revision floors",
          "[editor][preview][apply]") {
    PreviewMailbox mailbox;
    const auto image = [](u64 generation, u64 revision, u64 frame) {
        return editor::preview::PreviewFrame{{1, 1, generation, revision, frame, 0},
                                             std::vector<std::byte>(4)};
    };
    mailbox.offer(image(7, 8, 12), 7);
    mailbox.offer(image(7, 9, 13), 7);
    mailbox.offer(image(7, 8, 12), 7); // Older transport delivery cannot replace newer.
    CHECK_FALSE(mailbox.take(7, 7, 7, 8));
    auto newest = mailbox.take(7, 9, 7, 9);
    REQUIRE(newest);
    CHECK(newest->info.frame_id == 13);
    mailbox.offer(image(7, 10, 14), 7);
    CHECK_FALSE(mailbox.take(8, 9, 9, 9)); // Reload discards the old worker's image.
    mailbox.offer(image(7, 11, 15), 8);
    CHECK_FALSE(mailbox.take(8, 9, 9, 11));
    mailbox.offer(image(8, 12, 1), 8);
    CHECK_FALSE(mailbox.take(8, 13, 9, 13)); // Camera/edit image floor.
    mailbox.offer(image(8, 12, 2), 8);
    CHECK_FALSE(mailbox.take(8, 9, 13, 14)); // Never regress displayed revisions.
    mailbox.offer(image(8, 14, 3), 8);
    REQUIRE(mailbox.take(8, 9, 13, 15)); // Lagged completed drag frames remain useful.
    mailbox.offer(image(8, 16, 4), 8);
    CHECK_FALSE(mailbox.take(8, 15, 15, 15));
    mailbox.clear(); // Callback abandoned or document replaced before acknowledgement.
    CHECK_FALSE(mailbox.take(8, 16, 15, 16)); // Same revision cannot revive its old pixels.
}
TEST_CASE("Preview resolution follows displayed framebuffer pixels and DPI",
          "[editor][preview][viewport]") {
    CHECK(preview_pixel_extent({1332, 999}, {2494, 1371}, {2494, 1371}) == Extent2D{1332, 999});
    CHECK(preview_pixel_extent({652, 489}, {1600, 1000}, {1600, 1000}) == Extent2D{652, 489});
    CHECK(preview_pixel_extent({652, 489}, {1600, 1000}, {3200, 2000}) == Extent2D{1304, 978});
    CHECK(preview_pixel_extent({652.4F, 489.3F}, {1600, 1000}, {1600, 1000}) == Extent2D{652, 489});
    CHECK(preview_pixel_extent({6000, 4500}, {6000, 4500}, {6000, 4500}) == Extent2D{4096, 3072});
    CHECK(preview_pixel_extent({600, 450}, {1600, 1000}, {}).empty());
    CHECK(preview_pixel_extent({600, 450}, {0, 1000}, {1600, 1000}).empty());
    CHECK(preview_pixel_extent({std::numeric_limits<f32>::infinity(), 1}, {1, 1}, {1, 1}).empty());
    CHECK(preview_pixel_extent({std::numeric_limits<f32>::quiet_NaN(), 1}, {1, 1}, {1, 1}).empty());
    const auto extreme = preview_pixel_extent({std::numeric_limits<f32>::max(), 1},
                                              {std::numeric_limits<f32>::min(), 1}, {8192, 8192});
    CHECK(extreme.width <= preview_capacity.width);
    CHECK(extreme.height <= preview_capacity.height);
}
TEST_CASE("Preview fitting respects aspect and never magnifies an independent window capture",
          "[editor][preview][viewport]") {
    CHECK(fit_preview_extent({1920, 1080}, {1280, 960}) == Extent2D{1280, 720});
    CHECK(fit_preview_extent({900, 1600}, {640, 480}) == Extent2D{270, 480});
    CHECK(fit_preview_extent({200, 200}, {640, 480}) == Extent2D{200, 200});
    CHECK(fit_preview_extent({0, 100}, {640, 480}).empty());
    CHECK(fit_preview_extent({100, 100}, {}).empty());
    CHECK(fit_preview_extent({4000000000U, 3000000000U}, {4096, 4096}) == Extent2D{4096, 3072});
}
TEST_CASE("Viewport size messages are small checked requests rather than scene mutations",
          "[editor][preview][viewport]") {
    auto message = encode_viewport_size({1500, 1125});
    CHECK(message == "viewport\n1500 1125");
    auto decoded = decode_viewport_size(std::string_view(message).substr(9), preview_capacity);
    REQUIRE(decoded);
    CHECK(*decoded == Extent2D{1500, 1125});
    for (auto text : {"0 1", "1 0", "-1 12", "12 -1", "4097 10", "1 4097", "1", "1 1 junk", "1 1 ",
                      " 1 1", "1  1", "99999999999999 1", "nan 12", "2.5 2"})
        CHECK_FALSE(decode_viewport_size(text, preview_capacity));
    CHECK_FALSE(decode_viewport_size("640 480", {320, 240}));
}
