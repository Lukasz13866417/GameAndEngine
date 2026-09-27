#pragma once

#include <vng/input/routing.hpp>
#include <cmath>
#include <optional>

namespace editor_example {

// A parent-owned walk through one window's input, in receipt order. Each
// occurrence finishes its descent through the viewport before the next starts.
// The final empty step advances held controls once, even in an idle frame.
// Views remain valid until next(); children must not retain them.
class ViewportInputSteps final {
public:
    struct Step {
        const vng::input::Frame& frame;
        std::span<const vng::input::Event> unhandled;
        double seconds{};
        bool tick{};
    };

    ViewportInputSteps(const vng::input::Frame& source,std::span<const vng::input::Event> available,double seconds)
        : source_(source),available_(available),seconds_(std::isfinite(seconds)?std::max(0.,seconds):0.) {
        frame_.logical_size=source.logical_size;
        frame_.framebuffer=source.framebuffer;
        frame_.pointer=source.pointer;
        frame_.focused=source.focused;
        frame_.overflow=source.overflow;
        frame_.keys=source.keys;
        frame_.events.reserve(1);
        // Frame snapshots describe the end of the batch. Undo discrete state
        // transitions first, then expose each occurrence's actual state.
        using namespace vng::input;
        for(auto i=source.events.rbegin();i!=source.events.rend();++i) {
            const auto key=static_cast<std::size_t>(i->key);
            if(key<frame_.keys.size()&&i->key!=Key::unknown) {
                if(i->kind==EventKind::key_up)frame_.keys[key]=true;
                if(i->kind==EventKind::key_down&&!i->repeat)frame_.keys[key]=false;
            }
            if(i->kind==EventKind::focus_lost)frame_.focused=true;
            if(i->kind==EventKind::focus_gained)frame_.focused=false;
        }
    }

    [[nodiscard]] std::optional<Step> next() {
        using namespace vng::input;
        frame_.events.clear();
        if(index_<source_.events.size()) {
            const auto& event=source_.events[index_++];
            frame_.events.push_back(event);
            if(event.kind!=EventKind::focus_lost&&event.kind!=EventKind::focus_gained&&event.kind!=EventKind::text)
                frame_.pointer=event.position;
            const auto key=static_cast<std::size_t>(event.key);
            if(key<frame_.keys.size()&&event.key!=Key::unknown) {
                if(event.kind==EventKind::key_down)frame_.keys[key]=true;
                if(event.kind==EventKind::key_up)frame_.keys[key]=false;
            }
            if(event.kind==EventKind::focus_lost)frame_.focused=false;
            if(event.kind==EventKind::focus_gained)frame_.focused=true;
            return Step{frame_,available_.contains(event)?std::span<const Event>{frame_.events}:std::span<const Event>{},0,false};
        }
        if(ticked_)return {};
        ticked_=true;
        frame_.pointer=source_.pointer;
        frame_.focused=source_.focused;
        frame_.keys=source_.keys;
        return Step{frame_,{},seconds_,true};
    }

private:
    const vng::input::Frame& source_;
    vng::input::AvailableEvents available_;
    vng::input::Frame frame_;
    std::size_t index_{};
    double seconds_{};
    bool ticked_{};
};

} // namespace editor_example
