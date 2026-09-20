#pragma once

#include "animation.hpp"

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
        const auto previous_size = source_.size();
        source_.resize(instances.size());
        values_.resize(instances.size());
        for (std::size_t i = 0; i < instances.size(); ++i) {
            const bool changed = i >= previous_size || source_[i] != instances[i];
            if (changed) source_[i] = instances[i];
            if (resample || changed) {
                values_[i] = evaluate_instance(state, instances[i], time);
                ++evaluations_;
            }
        }
        time_ = time;
        animation_version_ = state.document.timeline.version();
        return values_;
    }
    [[nodiscard]] vng::u64 evaluations() const noexcept { return evaluations_; }
private:
    std::vector<SceneInstance> source_, values_;
    std::optional<vng::f32> time_;
    vng::u64 animation_version_{}, evaluations_{};
};
} // namespace editor_example
