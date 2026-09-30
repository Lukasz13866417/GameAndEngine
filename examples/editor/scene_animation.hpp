#pragma once

#include <vng/content/document.hpp>
#include <vng/timeline/timeline.hpp>
#include <array>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace editor_example {
struct State;
struct InstanceTransform;
struct SceneInstance;
enum class BlueprintId : vng::u32;

// Authored trees contain values, not pointers to editor components. Children
// return values; a concrete parent decides how and when to combine them.
struct AnimationInterval {
    vng::f32 first{}, last{10};
    bool contains(vng::f32 time) const { return time >= first && time <= last; }
    friend bool operator==(const AnimationInterval&, const AnimationInterval&) = default;
};
struct RouteCurve {
    std::array<vng::Vec3,4> points{};
    vng::Vec3 evaluate(vng::f32 parameter) const;
    friend bool operator==(const RouteCurve&, const RouteCurve&) = default;
};
struct SpeedProfile {
    vng::f32 initial{1}, final{4}, ramp{.5F}; // ramp: fraction of the interval
    vng::f32 evaluate(vng::f32 phase) const; // normalized distance, not velocity
    friend bool operator==(const SpeedProfile&, const SpeedProfile&) = default;
};
struct Turbulence {
    vng::f32 amplitude{}, frequency{1};
    vng::Vec3 evaluate(vng::f32 seconds, vng::f32 phase) const;
    friend bool operator==(const Turbulence&, const Turbulence&) = default;
};
struct MotionSample { vng::Vec3 position, rotation; };
struct ShipMotion {
    RouteCurve route;
    SpeedProfile speed;
    Turbulence turbulence;
    vng::Vec3 initial_rotation{};
    bool orient_to_path{true};
    MotionSample evaluate(vng::f32 seconds, vng::f32 duration) const;
    friend bool operator==(const ShipMotion&, const ShipMotion&) = default;
};
struct CameraFollow {
    vng::Vec3 offset{}, rotation{};
    bool look_at_ship{};
    MotionSample evaluate(const MotionSample& ship) const;
    friend bool operator==(const CameraFollow&, const CameraFollow&) = default;
};
struct DepartureSequence {
    vng::u32 ship{}, camera{}; // camera == 0: no camera output
    ShipMotion motion;
    CameraFollow follow;
    struct Result { MotionSample ship, camera; };
    Result evaluate(vng::f32 seconds, vng::f32 duration) const;
    friend bool operator==(const DepartureSequence&, const DepartureSequence&) = default;
};
struct SpinAnimation {
    vng::u32 target{};
    vng::Vec3 initial_rotation{}, degrees_per_second{0,10,0};
    vng::Vec3 evaluate(vng::f32 seconds) const;
    friend bool operator==(const SpinAnimation&, const SpinAnimation&) = default;
};
struct AnimationSettings {
    AnimationInterval interval;
    bool enabled{true};
    std::variant<DepartureSequence,SpinAnimation> root;
    friend bool operator==(const AnimationSettings&, const AnimationSettings&) = default;
};
struct AnimationTargets { vng::u32 object{}, camera{}; };

// A frame's derived output, shared by bulk instance sampling. Each active root
// is evaluated once; this does not own authored state or mutate its targets.
class AnimationFrame {
public:
    AnimationFrame() = default;
    AnimationFrame(std::span<const SceneInstance>, vng::f32 time);
    void update(std::span<const SceneInstance>, vng::f32 time);
    void apply(vng::u32 object, InstanceTransform&) const;
    std::size_t evaluated_roots() const { return evaluated_roots_; }
private:
    struct Pose { std::optional<vng::Vec3> position, rotation; };
    struct RootSample { AnimationSettings settings; std::map<vng::u32, Pose> poses; };
    std::map<vng::u32, Pose> poses_;
    std::map<vng::u32, RootSample> roots_;
    std::optional<vng::f32> time_;
    std::size_t evaluated_roots_{};
};

// Builtin blueprints are factories. No half-bound instance enters Document.
struct DepartureBlueprint {
    vng::content::Result<AnimationSettings> instantiate(const State&, AnimationTargets, AnimationInterval) const;
};
struct SpinBlueprint {
    vng::content::Result<AnimationSettings> instantiate(const State&, AnimationTargets, AnimationInterval) const;
};
bool is_animation_blueprint(BlueprintId);
const AnimationSettings* scene_animation(const State&, vng::u32 id);
std::vector<vng::timeline::Target> animation_outputs(const AnimationSettings&);
vng::content::Result<void> validate_scene_animations(const State&, vng::u32 replacement_id = 0,
                                                   const AnimationSettings* replacement = nullptr);
vng::content::Result<void> validate_animation_roots(std::span<const SceneInstance>,vng::f32 duration,
    vng::u32 replacement_id=0,const AnimationSettings* replacement=nullptr);
std::optional<vng::u32> animation_owner(const State&, const vng::timeline::Target&, vng::f32 time);
void sample_scene_animations(const State&, vng::u32 object, vng::f32 time, InstanceTransform&);
vng::content::Result<vng::u32> create_scene_animation(State&, BlueprintId, AnimationTargets, AnimationInterval);
vng::content::Result<void> bake_scene_animation(State&, vng::u32 id, vng::u32 samples = 121);
void write_scene_animation(std::ostream&, const AnimationSettings&);
AnimationSettings read_scene_animation(vng::content::Reader);
std::string animation_debug_string(const AnimationSettings&);
} // namespace editor_example
