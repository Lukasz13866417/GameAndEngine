#pragma once
#include <memory>
#include <vng/text/font.hpp>

namespace vng::ui {
// Colors are linear RGB with straight opacity. Dimensions are logical pixels.
struct ThemeValues {
    text::Font font;
    f32 font_size{18}, padding{10}, gap{8}, control_height{36}, radius{5}, border_width{1};
    Vec4 panel{.025F, .035F, .055F, .97F}, surface{.055F, .075F, .11F, 1};
    Vec4 hover{.09F, .13F, .19F, 1}, pressed{.025F, .13F, .21F, 1};
    Vec4 text{.85F, .9F, 1, 1}, muted{.35F, .43F, .55F, 1};
    Vec4 accent{.06F, .42F, .75F, 1}, border{.13F, .18F, .25F, 1}, focus{.18F, .65F, 1, 1};
    Vec4 selection{.08F, .28F, .5F, .8F};
    f32 scrollbar_width{14}, scrollbar_gap{4}, scrollbar_min_thumb{24};
    Vec4 scrollbar_track{.018F, .025F, .04F, 1};
    Vec4 scrollbar_thumb{.16F, .23F, .32F, 1};
    Vec4 scrollbar_hover{.27F, .38F, .49F, 1};
    Vec4 scrollbar_pressed{.16F, .48F, .69F, 1};
};
class Theme {
public:
    virtual ~Theme() = default;
    [[nodiscard]] virtual const ThemeValues& values() const noexcept = 0;
};
[[nodiscard]] inline ThemeValues dark_theme_values(text::Font font) {
    ThemeValues value;
    value.font = std::move(font);
    return value;
}
class BasicTheme final : public Theme {
public:
    explicit BasicTheme(ThemeValues values) : values_(std::move(values)) {}
    const ThemeValues& values() const noexcept override { return values_; }

private:
    ThemeValues values_;
};
[[nodiscard]] inline std::shared_ptr<const Theme> dark_theme(text::Font font) {
    return std::make_shared<BasicTheme>(dark_theme_values(std::move(font)));
}
} // namespace vng::ui
