#include "edit_clipboard.hpp"
#include <algorithm>
#include <cmath>

namespace editor_example {
namespace {
auto invalid(std::string message) {
    vng::content::Diagnostic error;
    error.message=std::move(message);
    return std::unexpected(std::move(error));
}
}
vng::content::Result<void> EditClipboard::copy_instance(const State& state,vng::u32 object) {
    return copy_instances(state,{&object,1});
}
vng::content::Result<void> EditClipboard::copy_keyframe(const State& state,vng::f32 time) {
    return copy_keyframes(state,{&time,1});
}
vng::content::Result<void> EditClipboard::copy_instances(const State& state,std::span<const vng::u32> ids) {
    if(ids.empty()) return invalid("Select scene instances to copy");
    std::vector<Instance> copies;
    for(auto id:ids) {
        if(std::ranges::any_of(copies,[&](const auto& c){return c.value.id==id;})) continue;
        const auto* instance=find_instance(state,id);
        if(!instance) return invalid("Select existing scene instances to copy");
        Instance copy{*instance,{}};
        for(const auto& track:state.document.timeline.tracks()) if(track.target.object==id) copy.tracks.push_back(track);
        copies.push_back(std::move(copy));
    }
    contents_=std::move(copies);return {};
}
vng::content::Result<void> EditClipboard::copy_keyframes(const State& state,std::span<const vng::f32> selected) {
    if(selected.empty()) return invalid("Select keyframes to copy");
    if(std::ranges::any_of(selected,[](vng::f32 time){return !std::isfinite(time);}))
        return invalid("Keyframe timestamps must be finite");
    std::vector<vng::f32> sorted(selected.begin(),selected.end());
    std::ranges::sort(sorted);sorted.erase(std::unique(sorted.begin(),sorted.end()),sorted.end());
    const auto times=keyframe_times(state);
    std::vector<Keyframe> copies;
    for(auto time:sorted) {
        if(std::ranges::find(times,time)==times.end()) return invalid("Select existing keyframes to copy");
        auto values=keyframe_values(state,time);
        std::erase_if(values,[](const auto& v){return !v.keyed;});
        std::vector<std::pair<vng::u32,bool>> orbits;
        for(const auto& value:values)
            if(value.target.property=="position" && value.target.object<=UINT32_MAX)
                if(const auto* lens=camera_settings(state,static_cast<vng::u32>(value.target.object)))
                    orbits.push_back({static_cast<vng::u32>(value.target.object),lens->orbit});
        copies.push_back({keyframe_name(state,time),std::move(values),time-sorted.front(),std::move(orbits)});
    }
    contents_=std::move(copies);return {};
}
vng::content::Result<EditClipboard::Pasted> EditClipboard::paste(State& state) const {
    if(std::holds_alternative<std::monostate>(contents_)) return invalid("Copy an instance or keyframe first");
    // Paste is one undoable authoring transaction, not a frame-by-frame edit.
    auto next=state;
    Pasted result;
    if(const auto* instances=std::get_if<std::vector<Instance>>(&contents_)) {
      for(const auto& value:*instances) {
        const auto* source=&value;
        auto created=instantiate(next,source->value.blueprint);
        if(!created) return std::unexpected(created.error());
        auto* instance=find_instance(next,*created);
        const auto default_name=instance->name;
        *instance=source->value;
        instance->id=*created;
        instance->name=source->value.name+" copy "+std::to_string(*created);
        if(instance->name.size()>256) instance->name=default_name;
        for(auto track:source->tracks) {
            track.target.object=*created;
            if(auto added=next.document.timeline.replace_track(std::move(track)); !added)
                return invalid(added.error().message);
        }
        next.viewport.mode=ViewMode::scene;
        result.object=*created;
        result.objects.push_back(*created);
      }
    } else {
      for(const auto& keyframe:std::get<std::vector<Keyframe>>(contents_)) {
        const auto time=state.viewport.time+keyframe.offset;
        if(!std::isfinite(time) || time<0 || time>state.document.timeline_duration)
            return invalid("Paste keyframes at a time inside the timeline");
        const auto times=keyframe_times(next);
        if(std::ranges::find(times,time)!=times.end())
            return invalid("Move the playhead to an empty timestamp before pasting a keyframe");
        for(const auto& value:keyframe.values)
            if(auto keyed=key_property(next,value.target,time,value.value,value.incoming); !keyed)
                return std::unexpected(keyed.error());
        // A camera that switched placement since the copy gets the same eye,
        // stored the other way.
        for(const auto& [camera,orbit]:keyframe.orbits) {
            const auto* instance=find_instance(next,camera);
            const auto* lens=instance?std::get_if<CameraSettings>(&instance->settings):nullptr;
            if(!lens || lens->orbit==orbit) continue;
            const auto value=std::ranges::find(keyframe.values,vng::timeline::Target{camera,"position"},&KeyframeValue::target);
            const auto placed=switch_placement(next,*instance,std::get<vng::Vec3>(value->value),time,lens->orbit);
            if(auto keyed=key_property(next,value->target,time,placed,value->incoming); !keyed)
                return std::unexpected(keyed.error());
        }
        next.document.keyframe_names[time]=keyframe.name;
        result.keyframe=time;
        result.keyframes.push_back(time);
      }
    }
    if(auto valid=validate_animation(next); !valid) return std::unexpected(valid.error());
    state=std::move(next);
    return result;
}
}
