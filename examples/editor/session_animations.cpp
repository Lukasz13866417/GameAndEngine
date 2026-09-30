#include "editing_session.hpp"

namespace editor_example {
using namespace vng;
content::Result<u32> EditingSession::create_animation(BlueprintId blueprint,AnimationTargets targets,AnimationInterval interval) {
    if(auto ready=available();!ready)return std::unexpected(ready.error());
    auto candidate=state_;
    auto id=create_scene_animation(candidate,blueprint,targets,interval);if(!id)return id;
    if(auto result=replace(std::move(candidate),{.full=true});!result)return std::unexpected(result.error());
    return *id;
}
content::Result<void> EditingSession::begin_animation(u32 id) {
    if(!scene_animation(state_,id)) {
        content::Diagnostic error;error.message="Select an animation instance";return std::unexpected(error);
    }
    DocumentChanges changes;changes.animations.insert(id);
    return begin(EditGesture::animation_tree,changes,{id});
}
content::Result<u32> EditingSession::preview_animation(BlueprintId blueprint,AnimationTargets targets,AnimationInterval interval) {
    if(auto ready=available();!ready)return std::unexpected(ready.error());
    if(state_.document.instances.size()>=instance_limit_) {
        content::Diagnostic error;error.message="Instance limit reached";return std::unexpected(error);
    }
    auto before=capture({.full=true});if(!before)return std::unexpected(before.error());
    auto id=create_scene_animation(state_,blueprint,targets,interval);if(!id)return id;
    gesture_=Gesture{EditGesture::animation_tree,std::move(*before),{*id},{},{},{},{},true};
    ++gesture_serial_;publish({.full=true},true);return *id;
}
content::Result<bool> EditingSession::animation(const AnimationSettings& value) {
    if(!active(EditGesture::animation_tree))return false;
    if(auto ready=writable();!ready)return std::unexpected(ready.error());
    const auto id=gesture_->objects.front();
    if(auto valid=validate_scene_animations(state_,id,&value);!valid)return std::unexpected(valid.error());
    if(*scene_animation(state_,id)==value)return false;
    if(creating_animation()) {
        find_instance(state_,id)->settings=value;
        DocumentChanges changes;changes.animations.insert(id);publish(changes);return true;
    }
    auto before=capture(gesture_->before.scope);if(!before)return std::unexpected(before.error());
    find_instance(state_,id)->settings=value;
    return updated(*before);
}
content::Result<bool> EditingSession::bake_animation(u32 id,u32 samples) {
    if(auto ready=available();!ready)return std::unexpected(ready.error());
    auto candidate=state_;
    if(auto result=bake_scene_animation(candidate,id,samples);!result)return std::unexpected(result.error());
    return replace(std::move(candidate),{.full=true});
}
}
