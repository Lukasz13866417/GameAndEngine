#pragma once
#include "world_bounds.hpp"
#include <vng/content/diagnostic.hpp>
#include <vng/ui/ui.hpp>
#include <charconv>

namespace editor_example {
class WorldBoundsPanel {
public:
    WorldBoundsPanel(vng::ui::Container controls, vng::ui::Container details)
        : host_(controls.row().height(36).padding(0).gap(4)),
          show_(host_.checkbox("World bounds").width(180)), frame_(host_.button("Frame world bounds").width(212)),
          values_(details.column().padding(0).gap(4)) {
        values_.label("Axis / min / max").height(24);
        for (unsigned axis = 0; axis < 3; ++axis) {
            auto row = values_.row().padding(0).gap(3).height(36);
            const std::string name(1, "XYZ"[axis]);
            row.label(name).width(28);
            fields_[axis*2] = row.text_input("World minimum " + name).width(84);
            fields_[axis*2+1] = row.text_input("World maximum " + name).width(84);
        }
        apply_ = values_.button("Apply world bounds");
        sync(WorldBounds{});
    }
    void sync(const WorldBounds& value) {
        if (current_ && *current_ == value) return;
        current_ = value;
        for (unsigned axis = 0; axis < 3; ++axis) for (unsigned side = 0; side < 2; ++side) {
            std::array<char, 64> bytes{};
            const auto [end, error] = std::to_chars(bytes.data(), bytes.data()+bytes.size(), side ? value.maximum[axis] : value.minimum[axis]);
            if (error == std::errc{}) fields_[axis*2+side].value(std::string_view{bytes.data(), end});
        }
    }
    void available(bool scene, bool editing) {
        host_.enabled(scene);
        frame_.enabled(editing);
        values_.visible(scene && show_.value()).enabled(editing);
    }
    bool visible() const { return show_.value(); }
    bool frame_requested() const { return frame_.clicked(); }
    void show() { show_.value(true); }
    bool apply_requested() const { return apply_.clicked(); }
    vng::content::Result<WorldBounds> read() const {
        WorldBounds bounds;
        const auto invalid = [] {
            vng::content::Diagnostic error;
            error.message = "World bounds need finite numbers with minimum < maximum on every axis";
            return std::unexpected(std::move(error));
        };
        for (unsigned axis = 0; axis < 3; ++axis) for (unsigned side = 0; side < 2; ++side) {
            auto source = fields_[axis*2+side].getText();
            const auto first = source.find_first_not_of(" \t");
            if (first == std::string_view::npos) return invalid();
            source = source.substr(first, source.find_last_not_of(" \t")-first+1);
            auto& value = side ? bounds.maximum[axis] : bounds.minimum[axis];
            const auto [end,error] = std::from_chars(source.data(),source.data()+source.size(),value);
            if (error != std::errc{} || end != source.data()+source.size()) return invalid();
        }
        if (!valid_world_bounds(bounds)) return invalid();
        return bounds;
    }
private:
    vng::ui::Container host_;
    vng::ui::Checkbox show_;
    vng::ui::Button frame_;
    vng::ui::Container values_;
    std::array<vng::ui::TextField,6> fields_{};
    vng::ui::Button apply_;
    std::optional<WorldBounds> current_;
};
}
