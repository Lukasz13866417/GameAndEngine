#pragma once
#include "settings.hpp"
#include "number_control.hpp"
#include <vng/ui/ui.hpp>
#include <array>
#include <vector>

namespace editor_example {
// A preference draft, separate from live camera-pose/keyframe controls.
class CameraPreferences {
public:
    explicit CameraPreferences(vng::ui::Container parent) {
        parent.label("NAVIGATION PREFERENCES").height(28);
        constexpr std::array labels{"Maximum viewing distance", "Minimum orbit distance", "Maximum orbit distance",
            "Walk forward speed (units/s)", "Walk sideways speed (units/s)", "Walk vertical speed (units/s)", "Walk Shift multiplier",
            "Shift + middle drag: pan multiplier", "Ctrl + middle drag: forward multiplier", "Middle drag: rotation (degrees/pixel)"};
        constexpr std::array captions{"View distance", "Min orbit", "Max orbit", "Walk forward", "Walk sideways",
            "Walk vertical", "Walk Shift", "Pan multiplier", "Forward multiplier", "Orbit deg/pixel"};
        constexpr std::array maximum{10'000'000.F,1'000'000.F,1'000'000.F,1'000'000.F,1'000'000.F,1'000'000.F,100.F,100.F,100.F,100.F};
        // Slider ranges cover everyday navigation; typed input retains the full
        // supported range (and the slider expands to show larger saved values).
        for(std::size_t i=0;i<labels.size();++i) {
            if(i==3)parent.label("Walk speeds (units / second)").height(24);
            if(i==7)parent.label("Middle drag: Shift pan / Ctrl forward").height(24);
            fields_.emplace_back(parent,labels[i],minimum[i],maximum[i],minimum[i],captions[i]);
            fields_.back().slider_range(minimum[i],slider_max[i]);
        }
        apply_=parent.button("Save camera preferences").height(36);
    }
    void show(const Settings& settings) {
        const std::array values{settings.maximum_viewing_distance,settings.orbit_distance.minimum,
            settings.orbit_distance.maximum,settings.walk.forward,settings.walk.sideways,
            settings.walk.vertical,settings.walk.fast_multiplier,settings.camera_drag.pan,settings.camera_drag.forward,settings.camera_drag.rotation};
        for(std::size_t i=0;i<values.size();++i) {
            fields_[i].reset(values[i]);
            fields_[i].slider_range(minimum[i],slider_max[i]);
        }
    }
    void poll() { for(auto& field:fields_)field.poll(); }
    bool applied() const { return apply_.clicked(); }
    vng::content::Result<Settings> read(Settings next) const {
        const std::array values{&next.maximum_viewing_distance,&next.orbit_distance.minimum,
            &next.orbit_distance.maximum,&next.walk.forward,&next.walk.sideways,&next.walk.vertical,&next.walk.fast_multiplier,
            &next.camera_drag.pan,&next.camera_drag.forward,&next.camera_drag.rotation};
        for(std::size_t i=0;i<values.size();++i) {
            auto value=fields_[i].read();
            if(!value) {
                vng::content::Diagnostic diagnostic; diagnostic.message=value.error().message;
                return std::unexpected(std::move(diagnostic));
            }
            *values[i]=*value;
        }
        if(auto valid=validate_settings(next);!valid) return std::unexpected(valid.error());
        return next;
    }
private:
    static constexpr std::array minimum{1.F,.001F,.001F,.001F,.001F,.001F,1.F,.001F,.001F,.001F};
    static constexpr std::array slider_max{100'000.F,10.F,100'000.F,100.F,100.F,100.F,10.F,10.F,10.F,2.F};
    std::vector<NumberControl> fields_;
    vng::ui::Button apply_;
};
}
