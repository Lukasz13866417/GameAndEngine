#pragma once
#include "settings.hpp"
#include <vng/ui/ui.hpp>
#include <algorithm>
#include <array>
#include <charconv>

namespace editor_example {
// UI-only preferences draft. Poll once after Screen::update(), passing raw
// events so Escape works inside fields and held Enter does not repeatedly save.
// The host handles modal input routing and closes only after persistence succeeds.
class SettingsPanel {
public:
    explicit SettingsPanel(vng::ui::Container panel) : panel_(panel) {
        panel_.padding(18).gap(7).scrollbar(vng::ui::ScrollBar::automatic);
        panel_.label("EDITOR SETTINGS").height(30);
        auto scale_row=panel_.row().height(36).padding(0).gap(8);
        scale_row.label("UI scale % (75–150)").width(450);
        scale_=scale_row.text_input("UI scale %").width(100);
        constexpr std::array labels{"UI FPS cap (0 = unlimited)", "Embedded preview FPS cap",
                                    "Independent Play FPS cap (0 = unlimited)",
                                    "Debug-preview FPS cap (0 = unlimited)", "Preview resolution % (100 = native)",
                                    "Minimum orbit distance", "Maximum orbit distance", "Timeline track limit", "Instance limit",
                                    "Maximum viewing distance", "Walk forward speed (units/s)",
                                    "Walk sideways speed (units/s)", "Walk vertical speed (units/s)", "Walk Shift multiplier"};
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            auto row = panel_.row().height(36).padding(0).gap(8);
            row.label(labels[i]).width(450);
            fields_[i] = row.text_input().width(100);
        }
        vsync_ = panel_.checkbox("VSync (editor and independent Play)").height(28);
        panel_.label("Preferences apply to this editor; they are not saved inside the scene.").height(28);
        status_ = panel_.label("").height(48);
        auto buttons = panel_.row().height(40).padding(0).gap(10);
        apply_ = buttons.button("Apply & save").width(160);
        cancel_ = buttons.button("Cancel").width(120);
        defaults_ = buttons.button("Defaults").width(120);
        panel_.visible(false);
    }
    void open(const Settings& value, std::size_t used_tracks = 0, std::size_t instances = 0) {
        set(value); visible_ = true; panel_.visible(true);
        status_.text(std::to_string(instances)+" instances / " + std::to_string(used_tracks) + " tracks. Limits affect additions only.");
    }
    void close() { visible_ = false; panel_.visible(false); }
    bool visible() const { return visible_; }
    void error(std::string_view text) { status_.text(text); }
    std::optional<Settings> poll(std::span<const vng::input::Event> raw) {
        using namespace vng;
        if (!visible_) return {};
        if (cancel_.clicked() || std::ranges::any_of(raw, [](const auto& event) {
                return event.kind == input::EventKind::key_down && event.key == input::Key::escape;
            })) { close(); return {}; }
        if (defaults_.clicked()) { set(Settings{}); status_.text("Defaults staged; Apply to save."); }
        const bool enter = (scale_.submittedText().has_value() || std::ranges::any_of(fields_, [](const auto& field) { return field.submittedText().has_value(); })) &&
                           std::ranges::any_of(raw, [](const auto& event) { return event.kind == input::EventKind::key_down && event.key == input::Key::enter && !event.repeat; });
        if (!apply_.clicked() && !enter) return {};
        Settings next=draft_;
        const auto scale_text=scale_.getText();
        const auto [scale_end,scale_error]=std::from_chars(scale_text.data(),scale_text.data()+scale_text.size(),next.ui_scale_percent);
        if(scale_error!=std::errc{} || scale_end!=scale_text.data()+scale_text.size()) {
            error("Enter a whole-number UI scale (75–150%)."); return {};
        }
        std::array<unsigned*, 5> values{&next.ui_fps, &next.preview_fps, &next.play_fps,
                                       &next.debug_fps, &next.preview_percent};
        for (std::size_t i = 0; i < values.size(); ++i) {
            auto text = fields_[i].getText();
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);
            const auto [end, e] = std::from_chars(text.data(), text.data() + text.size(), *values[i]);
            if (e != std::errc{} || end != text.data() + text.size() || text.empty()) {
                error("Enter whole numbers in all settings fields."); return {};
            }
        }
        const std::array distances{&next.orbit_distance.minimum, &next.orbit_distance.maximum};
        for (std::size_t i = 0; i < distances.size(); ++i) {
            auto text = fields_[i + 5].getText();
            const auto first = text.find_first_not_of(" \t");
            if (first != std::string_view::npos) text = text.substr(first, text.find_last_not_of(" \t") - first + 1);
            const auto [end, e] = std::from_chars(text.data(), text.data() + text.size(), *distances[i]);
            if (e != std::errc{} || end != text.data() + text.size() || text.empty()) {
                error("Enter numbers for the orbit-distance limits."); return {};
            }
        }
        const std::array limits{&next.timeline_track_limit, &next.instance_limit};
        for (std::size_t i = 0; i < limits.size(); ++i) {
            auto text = fields_[7 + i].getText();
            const auto first = text.find_first_not_of(" \t");
            if (first != std::string_view::npos)
                text = text.substr(first, text.find_last_not_of(" \t") - first + 1);
            const auto [end, parse_error] = std::from_chars(text.data(), text.data() + text.size(), *limits[i]);
            if (parse_error != std::errc{} || end != text.data() + text.size() || text.empty()) {
                error("Enter whole numbers for the track and instance limits."); return {};
            }
        }
        next.vsync = vsync_.value() ? vng::window::VSync::on : vng::window::VSync::off;
        const std::array camera_values{&next.maximum_viewing_distance,&next.walk.forward,
            &next.walk.sideways,&next.walk.vertical,&next.walk.fast_multiplier};
        for(std::size_t i=0;i<camera_values.size();++i) {
            auto text=fields_[9+i].getText();
            const auto first=text.find_first_not_of(" \t");
            if(first!=std::string_view::npos) text=text.substr(first,text.find_last_not_of(" \t")-first+1);
            const auto [end,e]=std::from_chars(text.data(),text.data()+text.size(),*camera_values[i]);
            if(e!=std::errc{} || end!=text.data()+text.size() || text.empty()) {
                error("Enter numbers for viewing distance and walk speeds."); return {};
            }
        }
        if (auto valid = validate_settings(next); !valid) { error(valid.error().message); return {}; }
        return next;
    }
private:
    void set(const Settings& s) {
        draft_=s;
        scale_.value(std::to_string(s.ui_scale_percent));
        const std::array values{s.ui_fps, s.preview_fps, s.play_fps, s.debug_fps, s.preview_percent};
        for (std::size_t i = 0; i < values.size(); ++i) fields_[i].value(std::to_string(values[i]));
        for (std::size_t i = 0; const auto distance : {s.orbit_distance.minimum, s.orbit_distance.maximum}) {
            std::array<char, 64> bytes{};
            const auto [end, error] = std::to_chars(bytes.data(), bytes.data() + bytes.size(), distance);
            if (error == std::errc{}) fields_[5 + i].value(std::string_view{bytes.data(), end});
            ++i;
        }
        fields_[7].value(std::to_string(s.timeline_track_limit));
        fields_[8].value(std::to_string(s.instance_limit));
        for(std::size_t i=0; const auto value : {s.maximum_viewing_distance,s.walk.forward,
                s.walk.sideways,s.walk.vertical,s.walk.fast_multiplier}) {
            std::array<char,64> bytes{};
            const auto [end,error]=std::to_chars(bytes.data(),bytes.data()+bytes.size(),value);
            if(error==std::errc{}) fields_[9+i].value(std::string_view{bytes.data(),end});
            ++i;
        }
        vsync_.value(s.vsync == vng::window::VSync::on);
    }
    vng::ui::Container panel_;
    std::array<vng::ui::TextField, 14> fields_;
    vng::ui::TextField scale_;
    vng::ui::Checkbox vsync_;
    vng::ui::Label status_;
    vng::ui::Button apply_, cancel_, defaults_;
    bool visible_{};
    Settings draft_;
};
} // namespace editor_example
