#include <vng/text/font.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace {

using Catch::Approx;

vng::text::Font test_font()
{
    auto loaded = vng::text::Font::load(VNG_TEST_FONT_PATH);
    if (!loaded) {
        FAIL("could not load bundled test font: " << loaded.error().message);
    }
    return *loaded;
}

} // namespace

TEST_CASE("fonts report missing and invalid files and default handles", "[text][font]")
{
    const vng::text::Font empty;
    CHECK_FALSE(empty.valid());
    CHECK_FALSE(static_cast<bool>(empty));
    CHECK(empty.id() == 0);
    auto layout = empty.layout("A", 20);
    REQUIRE_FALSE(layout);
    CHECK(layout.error().code == vng::text::ErrorCode::invalid_font);
    CHECK_FALSE(empty.rasterize(0, 20));

    const auto missing_path = std::string(VNG_TEST_FONT_PATH) + ".does-not-exist";
    auto missing = vng::text::Font::load(missing_path);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == vng::text::ErrorCode::file_read_failed);

    auto invalid = vng::text::Font::load(__FILE__);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == vng::text::ErrorCode::invalid_font);
}

TEST_CASE("font handles share ownership and retain stable cache identities", "[text][font]")
{
    auto original = test_font();
    auto copy = original;
    const auto identity = original.id();
    REQUIRE(identity != 0);
    CHECK(copy.id() == identity);
    original = {};
    CHECK(copy.valid());
    CHECK(copy.id() == identity);
    CHECK(copy.layout("still alive", 18).has_value());
    CHECK(test_font().id() != identity);
}

TEST_CASE("text layout validates UTF-8 and pixel sizes", "[text][font]")
{
    const auto font = test_font();
    for (const auto& text : {
             std::string("\xC0\xAF", 2),
             std::string("\xED\xA0\x80", 3),
             std::string("\xF4\x90\x80\x80", 4),
             std::string("\xE2\x82", 2),
             std::string("\x80", 1),
             std::string("\xE2x\xAC", 3),
         }) {
        auto result = font.layout(text, 20);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == vng::text::ErrorCode::invalid_utf8);
    }
    for (const auto size : {0U, 4097U, std::numeric_limits<vng::u32>::max()}) {
        auto result = font.layout("", size);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == vng::text::ErrorCode::invalid_size);
        CHECK_FALSE(font.rasterize(0, size));
    }
    CHECK(font.layout("A\xC3\xA9", 20).has_value());
    CHECK(font.layout("A", 1).has_value());
    CHECK(font.measure("A", 4096).has_value());
    CHECK_FALSE(font.rasterize(std::numeric_limits<vng::u32>::max(), 20));
}

TEST_CASE("text shaping preserves kerning ligatures and UTF-8 clusters", "[text][font]")
{
    const auto font = test_font();
    auto a = font.measure("A", 48);
    auto v = font.measure("V", 48);
    auto av = font.measure("AV", 48);
    REQUIRE(a);
    REQUIRE(v);
    REQUIRE(av);
    CHECK(av->width < a->width + v->width);

    auto ligature = font.layout("ffi", 32);
    REQUIRE(ligature);
    CHECK(ligature->glyphs.size() < 3);
    CHECK(ligature->metrics.width > 0.0F);

    auto unicode = font.layout("A\xC3\xA9", 24);
    REQUIRE(unicode);
    REQUIRE(unicode->glyphs.size() == 2);
    CHECK(unicode->glyphs[0].cluster == 0);
    CHECK(unicode->glyphs[1].cluster == 1);
    CHECK(unicode->glyphs[0].glyph_index != 0);
    CHECK(unicode->glyphs[1].glyph_index != 0);

    auto decomposed = font.measure("e\xCC\x81", 24);
    auto composed = font.measure("\xC3\xA9", 24);
    REQUIRE(decomposed);
    REQUIRE(composed);
    CHECK(decomposed->width == Approx(composed->width));

    // One Arabic run: HarfBuzz selects contextual forms and lam-alef ligation.
    auto arabic = font.layout("\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85", 32);
    REQUIRE(arabic);
    CHECK(arabic->glyphs.size() < 4);
    CHECK(arabic->metrics.width > 0.0F);
    CHECK(std::all_of(arabic->glyphs.begin(), arabic->glyphs.end(),
        [](const auto& glyph) { return glyph.glyph_index != 0; }));
}

TEST_CASE("text uses top-left line boxes with newline and tab handling", "[text][font]")
{
    const auto font = test_font();
    auto empty = font.layout("", 24);
    REQUIRE(empty);
    CHECK(empty->glyphs.empty());
    CHECK(empty->metrics.width == 0.0F);
    CHECK(empty->metrics.height == 0.0F);
    CHECK(empty->metrics.line_count == 0);

    auto lines = font.layout("A\r\nB\n", 24);
    REQUIRE(lines);
    REQUIRE(lines->glyphs.size() == 2);
    CHECK(lines->metrics.line_count == 3);
    CHECK(lines->metrics.height == Approx(3.0F * lines->metrics.line_height));
    CHECK(lines->glyphs[0].position.y == Approx(lines->metrics.baseline));
    CHECK(lines->glyphs[1].position.y
          == Approx(lines->metrics.baseline + lines->metrics.line_height));
    CHECK(lines->glyphs[0].position.x == 0.0F);
    CHECK(lines->glyphs[1].position.x == 0.0F);
    CHECK(lines->glyphs[1].cluster == 3);

    auto tab = font.measure("\t", 24);
    auto spaces = font.measure("    ", 24);
    REQUIRE(tab);
    REQUIRE(spaces);
    CHECK(tab->width == Approx(spaces->width));
    auto tabbed = font.layout(" \tB", 24);
    REQUIRE(tabbed);
    REQUIRE(tabbed->glyphs.size() == 2);
    CHECK(tabbed->glyphs[1].position.x == Approx(tab->width));
    CHECK(tabbed->glyphs[1].cluster == 2);
}

TEST_CASE("glyph rasterization yields tight grayscale coverage and empty spaces", "[text][font]")
{
    const auto font = test_font();
    auto text = font.layout("A ", 32);
    REQUIRE(text);
    REQUIRE(text->glyphs.size() == 2);
    auto a = font.rasterize(text->glyphs[0].glyph_index, 32);
    REQUIRE(a);
    CHECK(a->width > 0);
    CHECK(a->height > 0);
    CHECK(a->top > 0);
    CHECK(a->pixels.size() == static_cast<std::size_t>(a->width) * a->height);
    CHECK(std::any_of(a->pixels.begin(), a->pixels.end(),
        [](auto coverage) { return coverage == 255; }));
    CHECK(std::any_of(a->pixels.begin(), a->pixels.end(),
        [](auto coverage) { return coverage > 0 && coverage < 255; }));

    auto space = font.rasterize(text->glyphs[1].glyph_index, 32);
    REQUIRE(space);
    CHECK(space->pixels.empty());
    CHECK(space->width * space->height == 0);
}

TEST_CASE("changing font size synchronizes shaping and rasterization", "[text][font]")
{
    const auto font = test_font();
    auto small = font.layout("AV", 16);
    REQUIRE(small);
    REQUIRE_FALSE(small->glyphs.empty());
    auto original_bitmap = font.rasterize(small->glyphs[0].glyph_index, 16);
    REQUIRE(original_bitmap);

    auto large = font.measure("AV", 64);
    REQUIRE(large);
    CHECK(large->width > small->metrics.width * 3.0F);
    auto large_bitmap = font.rasterize(small->glyphs[0].glyph_index, 64);
    REQUIRE(large_bitmap);
    CHECK(large_bitmap->height > original_bitmap->height * 3U);

    auto small_again = font.layout("AV", 16);
    REQUIRE(small_again);
    CHECK(small_again->metrics.width == Approx(small->metrics.width));
    auto bitmap_again = font.rasterize(small->glyphs[0].glyph_index, 16);
    REQUIRE(bitmap_again);
    CHECK(bitmap_again->pixels == original_bitmap->pixels);
    CHECK(bitmap_again->width == original_bitmap->width);
    CHECK(bitmap_again->height == original_bitmap->height);
}
