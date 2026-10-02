#pragma once

#include "animation.hpp"
#include <set>

namespace editor_example {
// Runtime-owned, CPU-only derived values. Camera/selection/geometry are not
// dependencies. Compare small authored instance records because State also
// supports direct edits outside EditingSession (demos/tests/tools).
class SceneSamples {
public:
    std::span<const SceneInstance> sample(const State& state, vng::f32 time) {
        const bool resample = !time_ || *time_ != time ||
            animation_version_ != state.document.timeline.version();
        const auto& instances = state.document.instances;
        bool roots_changed{};
        std::set<vng::u32> affected;
        const auto invalidate=[&](const SceneInstance& root) {
            roots_changed=true;
            if(const auto* a=std::get_if<AnimationSettings>(&root.settings))
                for(const auto& target:animation_outputs(*a))affected.insert(static_cast<vng::u32>(target.object));
        };
        for(const auto& previous:source_)if(is_animation_blueprint(previous.blueprint)) {
            auto current=std::ranges::find(instances,previous.id,&SceneInstance::id);
            if(current==instances.end()||*current!=previous)invalidate(previous);
        }
        for(const auto& current:instances)if(is_animation_blueprint(current.blueprint)) {
            auto previous=std::ranges::find(source_,current.id,&SceneInstance::id);
            if(previous==source_.end()||*previous!=current)invalidate(current);
        }
        if (resample || roots_changed) animations_.update(instances,time);
        const auto previous_size = source_.size();
        source_.resize(instances.size());
        values_.resize(instances.size());
        for (std::size_t i = 0; i < instances.size(); ++i) {
            const bool changed = i >= previous_size || source_[i] != instances[i];
            if (changed) source_[i] = instances[i];
            if (resample || changed || affected.contains(instances[i].id)) {
                values_[i] = evaluate_instance(state, instances[i], time, &animations_);
                ++evaluations_;
            }
        }
        time_ = time;
        animation_version_ = state.document.timeline.version();
        return values_;
    }
    [[nodiscard]] vng::u64 evaluations() const noexcept { return evaluations_; }
private:
    AnimationFrame animations_;
    std::vector<SceneInstance> source_, values_;
    std::optional<vng::f32> time_;
    vng::u64 animation_version_{}, evaluations_{};
};
} // namespace editor_example
