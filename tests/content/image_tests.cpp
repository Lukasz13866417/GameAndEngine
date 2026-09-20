#include <vng/content/image.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

TEST_CASE("Image decoding preserves PNG channels and alpha", "[content][image]")
{
    constexpr unsigned char encoded[]{
        0x89, 0x50, 0x4e, 0x47, 0xd, 0xa, 0x1a, 0xa, 0, 0, 0, 0xd, 0x49, 0x48, 0x44, 0x52,
        0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0, 0x1f, 0x15, 0xc4, 0x89,
        0, 0, 0, 0xd, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x50, 0x70, 0x68, 0x38,
        0, 0, 3, 5, 1, 0xa1, 0x7f, 0x62, 4, 0x2b, 0, 0, 0, 0, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    const auto bytes = std::as_bytes(std::span(encoded));
    auto decoded = vng::content::decode_image(bytes);
    REQUIRE(decoded);
    CHECK(decoded->extent == vng::Extent2D{1, 1});
    CHECK(decoded->pixels == std::vector<std::byte>{std::byte{32}, std::byte{64}, std::byte{128}, std::byte{192}});
    auto limited = vng::content::decode_image(bytes, {.max_decoded_bytes = 3});
    REQUIRE_FALSE(limited);
    CHECK(limited.error().code == vng::content::ErrorCode::limit_exceeded);
    CHECK_FALSE(vng::content::decode_image(bytes.first(40)));
}

TEST_CASE("PPM image decoding handles comments and binary whitespace pixels", "[content][image]")
{
    std::string ppm = "P6\n# authored image\n2 1\n255\n";
    for (unsigned char sample : std::array<unsigned char, 6>{10, 32, 35, 0, 128, 255})
        ppm.push_back(static_cast<char>(sample));
    const auto bytes = std::as_bytes(std::span(ppm));
    auto decoded = vng::content::decode_image(bytes);
    REQUIRE(decoded);
    CHECK(decoded->extent == vng::Extent2D{2, 1});
    CHECK(decoded->pixels == std::vector<std::byte>{std::byte{10}, std::byte{32}, std::byte{35}, std::byte{255},
        std::byte{0}, std::byte{128}, std::byte{255}, std::byte{255}});
    auto truncated = vng::content::decode_image(bytes.first(bytes.size() - 1));
    REQUIRE_FALSE(truncated);
    CHECK(truncated.error().code == vng::content::ErrorCode::count_mismatch);
    CHECK_FALSE(vng::content::decode_image(bytes, {.max_file_bytes = 2}));
    CHECK_FALSE(vng::content::decode_image(bytes, {.max_dimension = 1}));
    CHECK_FALSE(vng::content::decode_image({}));
    const std::filesystem::path absent = "vng-deliberately-absent-image-72935.png";
    auto missing = vng::content::load_image(absent);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().path == absent);
}
