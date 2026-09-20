#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <vng/ui/ui.hpp>

namespace editor_example {
// A convenient editor control, deliberately outside the engine UI API. Poll once
// after Screen::update(); event accessors are then stable until the next poll.
// Slider drags update continuously. Text is a draft until Enter (or a caller
// explicitly reads it for an Apply button). Escape discards only the text draft.
class NumberControl {
public:
    NumberControl(vng::ui::Container parent, std::string_view label, vng::f32 minimum,
                  vng::f32 maximum, vng::f32 value, std::string_view slider_caption = {})
        : row_(parent.row().padding(0).gap(6)),
          slider_(row_.slider(slider_caption.empty()?label:slider_caption, minimum, maximum).width(116)),
          text_(row_.text_input(label).width(86)), minimum_(minimum), maximum_(maximum),
          slider_minimum_(minimum), slider_maximum_(maximum), label_(label) {
        validate_value(value);
        reset(value);
        layout();
    }

    void poll() {
        changed_.reset();
        committed_ = false;
        layout();
        if (text_.editCancelled()) {
            write_text();
            error_.clear();
        }
        if (const auto value = slider_.changedValue()) {
            value_ = *value;
            changed_ = value_;
            write_text();
            error_.clear();
        }
        committed_ = slider_.editCommitted();
        if (text_.submittedText()) {
            auto next = read();
            if (!next) {
                error_ = next.error().message;
                return;
            }
            if (*next != value_)
                changed_ = *next;
            value_ = *next;
            sync_slider();
            write_text();
            error_.clear();
            committed_ = true;
        }
    }

    [[nodiscard]] vng::f32 value() const noexcept { return value_; }
    // Keep an existing authored value displayable when user preferences narrow
    // the navigation range; do not silently rewrite scene camera tracks.
    NumberControl& slider_range(vng::f32 minimum, vng::f32 maximum) {
        slider_minimum_=minimum;slider_maximum_=maximum;
        // Range synchronization must not overwrite an in-flight slider drag.
        slider_.range(std::min(slider_minimum_,value_),std::max(slider_maximum_,value_));
        return *this;
    }
    // Authoritative local updates can arrive before an asynchronous inspector
    // schema with new limits. Display the value without clamping scene data or
    // widening input validation. An obsolete slider is disabled until its
    // schema catches up; typed edits still use read()'s declared bounds.
    // Synchronization does not replace an in-progress or unsent text draft.
    NumberControl& value(vng::f32 next) {
        validate_finite(next);
        const bool preserve = text_.isFocused() || text_.getText() != formatted_;
        value_ = next;
        if (!slider_.isPressed())
            sync_slider();
        if (!preserve)
            write_text();
        return *this;
    }
    // Use when deliberately replacing the edited object, not on revision updates.
    NumberControl& reset(vng::f32 next) {
        validate_finite(next);
        value_ = next;
        sync_slider();
        write_text();
        changed_.reset();
        committed_ = false;
        error_.clear();
        return *this;
    }
    [[nodiscard]] vng::ui::Result<vng::f32> read() const {
        auto source = text_.getText();
        while (!source.empty() && (source.front() == ' ' || source.front() == '\t'))
            source.remove_prefix(1);
        while (!source.empty() && (source.back() == ' ' || source.back() == '\t'))
            source.remove_suffix(1);
        if (!source.empty() && source.front() == '+')
            source.remove_prefix(1);
        vng::f32 result{};
        const auto [end, error] =
            std::from_chars(source.data(), source.data() + source.size(), result);
        if (source.empty() || error != std::errc{} || end != source.data() + source.size() ||
            !std::isfinite(result))
            return std::unexpected(vng::ui::Diagnostic{"Enter a valid number for " + label_});
        if (result < minimum_ || result > maximum_)
            return std::unexpected(vng::ui::Diagnostic{
                label_ + " must be between " + format(minimum_) + " and " + format(maximum_)});
        return result;
    }
    [[nodiscard]] std::optional<vng::f32> changedValue() const noexcept { return changed_; }
    [[nodiscard]] bool editCommitted() const noexcept { return committed_; }
    [[nodiscard]] bool isPressed() const noexcept { return slider_.isPressed(); }
    [[nodiscard]] bool editing() const noexcept {
        return slider_.isPressed() || text_.isFocused();
    }
    [[nodiscard]] std::string_view status() const noexcept { return error_; }
    NumberControl& enabled(bool value) { row_.enabled(value); return *this; }
    NumberControl& visible(bool value) { row_.visible(value); return *this; }

private:
    static std::string format(vng::f32 value) {
        std::array<char, 64> storage{};
        const auto [end, error] = std::to_chars(storage.data(), storage.data() + storage.size(), value);
        if (error != std::errc{})
            throw std::runtime_error("Cannot format editor number");
        return {storage.data(), end};
    }
    void validate_value(vng::f32 value) const {
        if (!std::isfinite(value) || value < minimum_ || value > maximum_)
            throw std::invalid_argument("NumberControl '" + label_ + "': value " + format(value) +
                " must be finite and between " + format(minimum_) + " and " + format(maximum_));
    }
    void validate_finite(vng::f32 value) const {
        if(!std::isfinite(value))
            throw std::invalid_argument("NumberControl '" + label_ + "': value must be finite");
    }
    void sync_slider() {
        slider_.range(std::min(slider_minimum_,value_),std::max(slider_maximum_,value_));
        slider_.value(value_);
        slider_.enabled(value_>=minimum_ && value_<=maximum_);
    }
    void write_text() {
        formatted_ = format(value_);
        if (text_.getText() != formatted_)
            text_.value(formatted_);
    }
    void layout() {
        const auto width = row_.bounds().width;
        const auto field_width = std::min(86.0F, width * .44F);
        slider_.width(std::max(0.0F, width - field_width - 6));
        text_.width(field_width);
    }
    vng::ui::Container row_;
    vng::ui::Slider slider_;
    vng::ui::TextField text_;
    vng::f32 minimum_{}, maximum_{}, value_{};
    vng::f32 slider_minimum_{},slider_maximum_{};
    std::string label_, formatted_, error_;
    std::optional<vng::f32> changed_;
    bool committed_{};
};
} // namespace editor_example
