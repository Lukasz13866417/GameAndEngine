#include "../../cmake/glfw/scroll_axes.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <limits>

namespace {
struct Device {
    XIValuatorClassInfo position{};
    XIScrollClassInfo scroll{};
    std::array<XIAnyClassInfo*, 2> classes{
        reinterpret_cast<XIAnyClassInfo*>(&position), reinterpret_cast<XIAnyClassInfo*>(&scroll)};
    Device() {
        position.type = XIValuatorClass;
        position.number = 3;
        position.value = 480;
        scroll.type = XIScrollClass;
        scroll.number = 3;
        scroll.scroll_type = XIScrollTypeVertical;
        scroll.increment = 120;
    }
    VngScrollAxes axes() { return vngScrollAxes(static_cast<int>(classes.size()), classes.data()); }
};
}

TEST_CASE("XI2 preserves the first fractional wheel delta and every following delta", "[window][scroll]") {
    Device device;
    auto axes = device.axes();
    unsigned char mask[]{0b00001011}; // X/Y included; valuator 2 is absent.
    double values[]{42, 70, 479};
    XIValuatorState event{1, mask, values};
    double sum = 0;
    for (int i = 1; i <= 120; ++i) {
        values[2] = 480 - i;
        const auto delta = vngScrollDelta(&axes.vertical, &event);
        CHECK(delta == Catch::Approx(1.0 / 120));
        sum += delta;
        CHECK(vngScrollDelta(&axes.horizontal, &event) == 0);
    }
    CHECK(sum == Catch::Approx(1));
    values[2] += .25;
    CHECK(vngScrollDelta(&axes.vertical, &event) == Catch::Approx(-.25 / 120));
    // Repeated values, ordinary pointer motion, and missing mask bytes do not zoom.
    CHECK(vngScrollDelta(&axes.vertical, &event) == 0);
    mask[0] = 0b11;
    CHECK(vngScrollDelta(&axes.vertical, &event) == 0);
    event.mask_len = 0;
    CHECK(vngScrollDelta(&axes.vertical, &event) == 0);
}

TEST_CASE("XI2 does not count smooth scroll emulation twice or suppress legacy wheels", "[window][scroll]") {
    Device device;
    const auto axes = device.axes();
    for (int button : {4, 5}) {
        CHECK_FALSE(vngScrollButton(&axes, button, XIPointerEmulated));
        CHECK(vngScrollButton(&axes, button, 0)); // XTEST/legacy physical wheel.
    }
    CHECK(vngScrollButton(&axes, 6, XIPointerEmulated)); // No horizontal scroll class.
    CHECK(vngScrollButton(&axes, 7, XIPointerEmulated));
    const auto legacy = vngScrollAxes(0, nullptr);
    CHECK(vngScrollButton(&legacy, 4, XIPointerEmulated));
}

TEST_CASE("XI2 chooses a valid preferred axis and resets baselines with device classes", "[window][scroll]") {
    Device first;
    Device preferred;
    preferred.scroll.flags = XIScrollFlagPreferred;
    preferred.position.number = preferred.scroll.number = 9;
    preferred.position.value = 2000;
    preferred.scroll.increment = -10;
    std::array classes{first.classes[0], first.classes[1], preferred.classes[0], preferred.classes[1]};
    auto axes = vngScrollAxes(4, classes.data());
    CHECK(axes.vertical.number == 9);
    unsigned char mask[]{0, 0b10};
    double value = 2000.125;
    XIValuatorState event{2, mask, &value};
    CHECK(vngScrollDelta(&axes.vertical, &event) == Catch::Approx(.0125));

    preferred.position.value = 9000; // Re-entry/device change supplies a new baseline.
    axes = preferred.axes();
    value = 9000.125;
    CHECK(vngScrollDelta(&axes.vertical, &event) == Catch::Approx(.0125));
    value = std::numeric_limits<double>::quiet_NaN();
    CHECK(vngScrollDelta(&axes.vertical, &event) == 0);
    value = 9000.25;
    CHECK(vngScrollDelta(&axes.vertical, &event) == Catch::Approx(.0125));

    preferred.scroll.increment = 0;
    CHECK(preferred.axes().vertical.number == -1);
    preferred.scroll.increment = std::numeric_limits<double>::infinity();
    CHECK(preferred.axes().vertical.number == -1);
    preferred.scroll.increment = 120;
    preferred.position.value = std::numeric_limits<double>::quiet_NaN();
    CHECK(preferred.axes().vertical.number == -1);
}

TEST_CASE("XI2 keeps horizontal and vertical wheel units independent", "[window][scroll]") {
    Device horizontal, vertical;
    horizontal.scroll.scroll_type = XIScrollTypeHorizontal;
    horizontal.position.number = horizontal.scroll.number = 2;
    horizontal.scroll.increment = 40;
    std::array classes{horizontal.classes[0], horizontal.classes[1], vertical.classes[0], vertical.classes[1]};
    auto axes = vngScrollAxes(4, classes.data());
    unsigned char mask[]{0b1100};
    double values[]{479.5, 481};
    XIValuatorState event{1, mask, values};
    CHECK(vngScrollDelta(&axes.horizontal, &event) == Catch::Approx(.5 / 40));
    CHECK(vngScrollDelta(&axes.vertical, &event) == Catch::Approx(-1.0 / 120));
    CHECK_FALSE(vngScrollButton(&axes, 6, XIPointerEmulated));
    CHECK_FALSE(vngScrollButton(&axes, 7, XIPointerEmulated));
}
