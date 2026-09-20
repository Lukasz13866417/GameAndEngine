#pragma once
#include "editor_layout.hpp"
#include <optional>
#include <array>
#include <numeric>

namespace editor_example {
// Private UI proportions, not scene settings. The stateless editor layout still
// supplies defaults; user choices survive window, DPI and sidebar-tab changes.
class SidebarSizing {
public:
    struct Split {
        vng::f32 total, minimum, maximum;
        vng::f32 clamp(vng::f32 value) const { return std::clamp(value, minimum, maximum); }
    };
    static Split panels(const EditorLayout& layout) {
        const auto total = std::max(1.F, layout.inspector.height - 12);
        return {total, std::min(120.F, total*.4F), total-std::min(64.F, total*.3F)};
    }
    // Instances, regions, blueprints, manipulation tools, region creation.
    static constexpr std::size_t section_count = 5;
    using Heights = std::array<vng::f32, section_count>;
    static constexpr Heights minimum{100, 82, 100, 110, 148};
    Heights sections(const EditorLayout& layout) const {
        constexpr auto minimum_total = 540.F;
        // Outer padding, four grips and the eight gaps between sections/grips.
        const auto extra = std::max(120.F, layout.scene.height - 24 - 40 - 48 - minimum_total);
        auto heights = minimum;
        for (std::size_t i=0; i<section_count; ++i) heights[i] += weights_[i]*extra;
        return heights;
    }
    Split divider(std::size_t index, const EditorLayout& layout) const {
        const auto heights = sections(layout);
        const auto total = heights.at(index) + heights.at(index+1);
        return {total, minimum.at(index), total-minimum.at(index+1)};
    }
    void resize_section(std::size_t index, vng::f32 height, const EditorLayout& layout) {
        auto heights = sections(layout);
        const auto split = divider(index, layout);
        heights.at(index) = split.clamp(height);
        heights.at(index+1) = split.total-heights[index];
        vng::f32 extra{};
        for (std::size_t i=0; i<section_count; ++i) extra += heights[i]-minimum[i];
        if (extra <= 0) return;
        for (std::size_t i=0; i<section_count; ++i) weights_[i]=(heights[i]-minimum[i])/extra;
    }
    void apply(EditorLayout& layout) const {
        if (!scene_fraction_) return;
        const auto split = panels(layout);
        layout.scene.height = split.clamp(*scene_fraction_ * split.total);
        layout.keyframes.y = layout.scene.y + layout.scene.height + 12;
        layout.keyframes.height = split.total - layout.scene.height;
    }
    void resize_panels(vng::f32 height, const EditorLayout& layout) {
        const auto split = panels(layout);
        scene_fraction_ = split.clamp(height) / split.total;
    }
private:
    std::optional<vng::f32> scene_fraction_;
    Heights weights_{.35F,.15F,.3F,.1F,.1F};
};
}
