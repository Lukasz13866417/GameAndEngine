#include "scene_animation.hpp"
#include "animation.hpp"
#include "rotation_math.hpp"
#include "authoring_limits.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <sstream>

namespace editor_example {
using namespace vng;
namespace {
auto invalid(std::string message) {
    content::Diagnostic error; error.code=content::ErrorCode::invalid_document;
    error.message=std::move(message); return std::unexpected(std::move(error));
}
Vec3 add(Vec3 a,Vec3 b) { for(unsigned i=0;i<3;++i)a[i]+=b[i]; return a; }
Vec3 sub(Vec3 a,Vec3 b) { for(unsigned i=0;i<3;++i)a[i]-=b[i]; return a; }
Vec3 mul(Vec3 a,float s) { for(unsigned i=0;i<3;++i)a[i]*=s; return a; }
float length(Vec3 a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); }
bool finite(Vec3 a) { return valid_scene_position(a); }
bool range(float v,float lo,float hi) { return std::isfinite(v)&&v>=lo&&v<=hi; }
bool angles(Vec3 v) {return range(v.x,-360,360)&&range(v.y,-360,360)&&range(v.z,-360,360);}
Vec3 heading(Vec3 direction,Vec3 fallback) {
    const auto n=length(direction); if(n<1e-6F)return fallback;
    constexpr float deg=180/std::numbers::pi_v<float>;
    // Undo the preserved roll before solving Rz * Ry * Rx's forward axis.
    // Pick the equivalent pitch/yaw branch nearest the captured orientation,
    // so initially banked or upside-down targets do not jump on creation.
    const auto roll=fallback.z/deg,c=std::cos(roll),s=std::sin(roll);
    const Vec3 unrolled{c*direction.x+s*direction.y,-s*direction.x+c*direction.y,direction.z};
    const auto pitch=std::asin(std::clamp(unrolled.y/n,-1.F,1.F))*deg;
    const auto yaw=std::atan2(-unrolled.x,-unrolled.z)*deg;
    const auto a=rotation_math::near({pitch,yaw,fallback.z},fallback);
    const auto b=rotation_math::near({180-pitch,yaw+180,fallback.z},fallback);
    return length(sub(a,fallback))<=length(sub(b,fallback))?a:b;
}
float arc_parameter(const RouteCurve& route,float distance) {
    std::array<float,65> lengths{}; auto previous=route.points[0];
    for(unsigned i=1;i<lengths.size();++i) {
        auto p=route.evaluate(static_cast<float>(i)/64);
        lengths[i]=lengths[i-1]+length(sub(p,previous));previous=p;
    }
    const auto wanted=distance*lengths.back();
    for(unsigned i=1;i<lengths.size();++i)if(lengths[i]>=wanted) {
        const auto segment=lengths[i]-lengths[i-1];
        return (static_cast<float>(i-1)+(segment>1e-7F?(wanted-lengths[i-1])/segment:0))/64;
    }
    return 1;
}
}
Vec3 RouteCurve::evaluate(float u) const {
    auto p=points;u=std::clamp(u,0.F,1.F);
    for(unsigned size=3;size;--size)for(unsigned i=0;i<size;++i)
        p[i]=add(mul(p[i],1-u),mul(p[i+1],u));
    return p[0];
}
float SpeedProfile::evaluate(float phase) const {
    const auto integral=[&](float t) {
        const auto x=std::min(t/ramp,1.F);
        return initial*t+(final-initial)*(ramp*(x*x*x-.5F*x*x*x*x)+std::max(0.F,t-ramp));
    };
    return integral(std::clamp(phase,0.F,1.F))/integral(1);
}
Vec3 Turbulence::evaluate(float seconds,float phase) const {
    const auto envelope=std::sin(std::numbers::pi_v<float>*phase);
    return {amplitude*envelope*std::sin(seconds*frequency*2.3F),
        amplitude*envelope*std::sin(seconds*frequency*1.7F),0};
}
MotionSample ShipMotion::evaluate(float seconds,float duration) const {
    const auto phase=std::clamp(seconds/duration,0.F,1.F);
    const auto u=arc_parameter(route,speed.evaluate(phase));
    const auto p=route.evaluate(u);
    const auto tangent=sub(route.evaluate(std::min(1.F,u+.001F)),route.evaluate(std::max(0.F,u-.001F)));
    return {add(p,turbulence.evaluate(seconds,phase)),orient_to_path?heading(tangent,initial_rotation):initial_rotation};
}
MotionSample CameraFollow::evaluate(const MotionSample& ship) const {
    const auto p=add(ship.position,offset);
    return {p,look_at_ship?heading(sub(ship.position,p),rotation):rotation};
}
DepartureSequence::Result DepartureSequence::evaluate(float seconds,float duration) const {
    const auto moving=motion.evaluate(seconds,duration);
    // Explicit parent coordination: follow consumes this result, not a lookup
    // into another component or last frame's mutable scene.
    return {moving,follow.evaluate(moving)};
}
Vec3 SpinAnimation::evaluate(float seconds) const {
    auto result=initial_rotation;
    for(unsigned i=0;i<3;++i)result[i]=std::remainder(result[i]+degrees_per_second[i]*seconds,360.F);
    return result;
}
AnimationFrame::AnimationFrame(std::span<const SceneInstance> instances, float time) {
    update(instances,time);
}
void AnimationFrame::update(std::span<const SceneInstance> instances, float time) {
    std::set<u32> present;
    poses_.clear();
    for (const auto& instance : instances) {
        const auto* a = std::get_if<AnimationSettings>(&instance.settings);
        if (!a || !a->enabled || !a->interval.contains(time)) continue;
        present.insert(instance.id);
        auto [it,inserted] = roots_.try_emplace(instance.id);
        auto& cached = it->second;
        if (inserted || time_ != time || cached.settings != *a) {
            ++evaluated_roots_;
            cached.settings = *a;
            cached.poses.clear();
            if (const auto* d = std::get_if<DepartureSequence>(&a->root)) {
                const auto result = d->evaluate(time-a->interval.first,a->interval.last-a->interval.first);
                auto& ship = cached.poses[d->ship];
                ship.position = result.ship.position;
                if (d->motion.orient_to_path) ship.rotation = result.ship.rotation;
                if (d->camera) cached.poses[d->camera] = {result.camera.position,result.camera.rotation};
            } else {
                const auto& spin = std::get<SpinAnimation>(a->root);
                cached.poses[spin.target].rotation = spin.evaluate(time-a->interval.first);
            }
        }
        for (const auto& [id,pose] : cached.poses) {
            auto& combined = poses_[id];
            if (pose.position) combined.position = pose.position;
            if (pose.rotation) combined.rotation = pose.rotation;
        }
    }
    std::erase_if(roots_,[&](const auto& pair){return !present.contains(pair.first);});
    time_ = time;
}
void AnimationFrame::apply(u32 object, InstanceTransform& transform) const {
    if (const auto it = poses_.find(object); it != poses_.end()) {
        if (it->second.position) transform.position = *it->second.position;
        if (it->second.rotation) transform.rotation = *it->second.rotation;
    }
}
bool is_animation_blueprint(BlueprintId id) { return id==BlueprintId::departure||id==BlueprintId::spin; }
const AnimationSettings* scene_animation(const State& state,u32 id) {
    auto* instance=find_instance(state,id);return instance?std::get_if<AnimationSettings>(&instance->settings):nullptr;
}
std::vector<timeline::Target> animation_outputs(const AnimationSettings& value) {
    if(const auto* d=std::get_if<DepartureSequence>(&value.root)) {
        std::vector<timeline::Target> result{{d->ship,"position"}};
        if(d->motion.orient_to_path)result.push_back({d->ship,"rotation"});
        if(d->camera) {result.push_back({d->camera,"position"});result.push_back({d->camera,"rotation"});}
        return result;
    }
    return {{std::get<SpinAnimation>(value.root).target,"rotation"}};
}
content::Result<void> validate_scene_animations(const State& state,u32 replacement_id,const AnimationSettings* replacement) {
    return validate_animation_roots(state.document.instances,state.document.timeline_duration,replacement_id,replacement);
}
content::Result<void> validate_animation_roots(std::span<const SceneInstance> instances,float duration,u32 replacement_id,const AnimationSettings* replacement) {
    std::map<u32,const SceneInstance*> objects;
    for(const auto& instance:instances)objects.emplace(instance.id,&instance);
    const auto find=[&](u32 id)->const SceneInstance*{auto it=objects.find(id);return it==objects.end()?nullptr:it->second;};
    struct Root {u32 id;const AnimationSettings* value;std::vector<timeline::Target> writes;};
    std::vector<Root> roots;
    for(const auto& instance:instances) {
        const auto* value=instance.id==replacement_id&&replacement?replacement:std::get_if<AnimationSettings>(&instance.settings);
        if(!value)continue;
        if(!is_animation_blueprint(instance.blueprint)||
           (instance.blueprint==BlueprintId::departure)!=std::holds_alternative<DepartureSequence>(value->root))
            return invalid("Animation root does not match its blueprint");
        roots.push_back({instance.id,value,animation_outputs(*value)});
    }
    if(replacement&&std::ranges::none_of(roots,[&](const auto& r){return r.id==replacement_id;}))
        roots.push_back({replacement_id,replacement,animation_outputs(*replacement)});
    if(roots.size()>max_animation_roots)return invalid("At most "+std::to_string(max_animation_roots)+" animation roots are supported");
    for(const auto& root:roots) {
        const auto& a=*root.value;
        if(!range(a.interval.first,0,duration)||
           !range(a.interval.last,0,duration)||a.interval.last-a.interval.first<.001F)
            return invalid("Animation interval must span at least 0.001 seconds inside the timeline");
        for(const auto& target:root.writes) {
            const auto* object=find(static_cast<u32>(target.object));
            if(!object||std::holds_alternative<AnimationSettings>(object->settings))
                return invalid("Animation target is missing or is another animation root");
        }
        if(const auto* d=std::get_if<DepartureSequence>(&a.root)) {
            if(d->camera&&(d->camera==d->ship||!find(d->camera)||!std::holds_alternative<CameraSettings>(find(d->camera)->settings)))
                return invalid("Camera follow requires a distinct scene camera");
            if(!range(d->motion.speed.initial,.001F,10000)||!range(d->motion.speed.final,.001F,10000)||
               !range(d->motion.speed.ramp,.001F,1)||!range(d->motion.turbulence.amplitude,0,10000)||
               !range(d->motion.turbulence.frequency,0,100)||!finite(d->follow.offset)||
               !angles(d->follow.rotation)||!angles(d->motion.initial_rotation))return invalid("Invalid departure parameters");
            for(auto p:d->motion.route.points) {
                if(!finite(p))return invalid("Route point exceeds scene coordinates");
                for(unsigned axis=0;axis<3;++axis) {
                    const auto margin=axis<2?d->motion.turbulence.amplitude:0.F;
                    if(std::abs(p[axis])+margin>scene_coordinate_limit||
                       (d->camera&&std::abs(p[axis]+d->follow.offset[axis])+margin>scene_coordinate_limit))
                        return invalid("Route with turbulence / camera offset exceeds scene coordinates");
                }
            }
            if(length(sub(d->motion.route.points.back(),d->motion.route.points.front()))<.001F)
                return invalid("Departure route endpoints must differ");
        } else {
            const auto& spin=std::get<SpinAnimation>(a.root);
            if(!angles(spin.initial_rotation)||!finite(spin.degrees_per_second)||length(spin.degrees_per_second)>3600)
                return invalid("Invalid spin parameters (maximum 3600 degrees per second)");
        }
    }
    // Only writers of the same property can conflict. Independent roots do
    // not require pairwise comparisons (large asteroid fields have hundreds).
    std::map<std::pair<u64,std::string>,std::vector<const Root*>> writers;
    for(const auto& root:roots)if(root.value->enabled)
        for(const auto& output:root.writes)writers[{output.object,output.property}].push_back(&root);
    for(auto& [target,list]:writers) {
        std::ranges::sort(list,{},[](const Root* root){return root->value->interval.first;});
        for(std::size_t i=1;i<list.size();++i)
            if(list[i-1]->value->interval.last>=list[i]->value->interval.first)
                return invalid("Animation roots #"+std::to_string(list[i-1]->id)+" and #"+std::to_string(list[i]->id)+
                    " both control #"+std::to_string(target.first)+" / "+target.second+" in overlapping intervals");
    }
    return {};
}
std::optional<u32> animation_owner(const State& state,const timeline::Target& target,float time) {
    for(const auto& instance:state.document.instances)if(const auto* a=std::get_if<AnimationSettings>(&instance.settings)) {
        if(!a->enabled||!a->interval.contains(time))continue;
        for(const auto& output:animation_outputs(*a))if(output==target)return instance.id;
    }
    return {};
}
void sample_scene_animations(const State& state,u32 id,float time,InstanceTransform& transform) {
    for(const auto& instance:state.document.instances)if(const auto* a=std::get_if<AnimationSettings>(&instance.settings)) {
        if(!a->enabled||!a->interval.contains(time))continue;
        if(const auto* d=std::get_if<DepartureSequence>(&a->root)) {
            if(id!=d->ship&&id!=d->camera)continue;
            const auto value=d->evaluate(time-a->interval.first,a->interval.last-a->interval.first);
            const auto pose=id==d->ship?value.ship:value.camera;
            transform.position=pose.position;
            if(id!=d->ship||d->motion.orient_to_path)transform.rotation=pose.rotation;
        } else {
            const auto& spin=std::get<SpinAnimation>(a->root);
            if(id==spin.target)transform.rotation=spin.evaluate(time-a->interval.first);
        }
    }
}
content::Result<AnimationSettings> DepartureBlueprint::instantiate(const State& state,AnimationTargets targets,AnimationInterval interval) const {
    const auto* object=find_instance(state,targets.object);
    if(!object||is_animation_blueprint(object->blueprint))return invalid("Select the ship to animate first");
    const auto pose=evaluate_transform(state,*object,interval.first);
    DepartureSequence d;d.ship=targets.object;d.camera=targets.camera;d.motion.initial_rotation=pose.rotation;
    const auto direction=rotation_math::direction(pose.rotation,{0,0,-1});
    const auto distance=std::max(10.F,pose.scale*40);
    for(unsigned i=0;i<4;++i)d.motion.route.points[i]=add(pose.position,mul(direction,distance*static_cast<float>(i)/3));
    if(targets.camera) {
        const auto* camera=find_instance(state,targets.camera);
        if(!camera||!is_camera_instance(state,targets.camera))return invalid("Choose a scene camera, or None");
        const auto view=evaluate_transform(state,*camera,interval.first);
        d.follow.offset=sub(view.position,pose.position);d.follow.rotation=view.rotation;
    }
    AnimationSettings value{interval,true,d};
    if(auto valid=validate_scene_animations(state,state.document.next_instance_id,&value);!valid)return std::unexpected(valid.error());
    return value;
}
content::Result<AnimationSettings> SpinBlueprint::instantiate(const State& state,AnimationTargets targets,AnimationInterval interval) const {
    const auto* object=find_instance(state,targets.object);
    if(!object||is_animation_blueprint(object->blueprint))return invalid("Select the object to rotate first");
    AnimationSettings value{interval,true,SpinAnimation{targets.object,evaluate_transform(state,*object,interval.first).rotation}};
    if(auto valid=validate_scene_animations(state,state.document.next_instance_id,&value);!valid)return std::unexpected(valid.error());
    return value;
}
content::Result<u32> create_scene_animation(State& state,BlueprintId blueprint,AnimationTargets targets,AnimationInterval interval) {
    if(!is_animation_blueprint(blueprint))return invalid("Not an animation blueprint");
    if(!state.document.next_instance_id||state.document.next_instance_id==UINT32_MAX||state.document.instances.size()>=max_scene_instances)
        return invalid("Scene instance limit reached");
    auto value=blueprint==BlueprintId::departure?DepartureBlueprint{}.instantiate(state,targets,interval):SpinBlueprint{}.instantiate(state,targets,interval);
    if(!value)return std::unexpected(value.error());
    const auto id=state.document.next_instance_id++;
    state.document.instances.push_back({id,blueprint,(blueprint==BlueprintId::departure?"Departure ":"Spin ")+std::to_string(id),*value,{}});
    state.viewport.selected_object=id;return id;
}
content::Result<void> bake_scene_animation(State& state,u32 id,u32 samples) {
    const auto* source=scene_animation(state,id);
    if(!source||!source->enabled)return invalid("Select an enabled animation to bake");
    if(samples<2||samples>1024)return invalid("Bake sample count must be between 2 and 1024");
    const auto a=*source;auto timeline=state.document.timeline;
    for(const auto& output:animation_outputs(a)) {
        const auto* target=find_instance(state,static_cast<u32>(output.object));
        auto track=timeline.find(output)?*timeline.find(output):vng::timeline::Track{output,output.property,"Baked animation",{}};
        const auto base=output.property=="position"?target->transform.position:target->transform.rotation;
        const auto raw=[&](float t) {auto v=state.document.timeline.sample(output,t);return v?std::get<Vec3>(*v):base;};
        const auto mode_after=[&](float t) {for(const auto& key:track.keys)if(key.time>t)return key.incoming;return vng::timeline::Interpolation::hold;};
        const auto before_mode=mode_after(a.interval.first);
        std::erase_if(track.keys,[&](const auto& key){return a.interval.contains(key.time);});
        auto insert=[&](float t,Vec3 value,vng::timeline::Interpolation mode) {
            std::erase_if(track.keys,[&](const auto& key){return key.time==t;});track.keys.push_back({t,value,mode});
        };
        if(a.interval.first>0) {
            if(track.keys.empty()||std::ranges::none_of(track.keys,[](const auto& k){return k.time==0;}))
                insert(0,raw(0),vng::timeline::Interpolation::hold);
            const auto t=std::nextafter(a.interval.first,0.F);insert(t,raw(t),before_mode);
        }
        std::optional<Vec3> previous_rotation;
        for(u32 i=0;i<samples;++i) {
            const auto t=i+1==samples?a.interval.last:a.interval.first+(a.interval.last-a.interval.first)*static_cast<float>(i)/static_cast<float>(samples-1);
            const auto value=evaluate_transform(state,*target,t);
            if(output.property=="rotation") {
                if(previous_rotation)for(unsigned c=0;c<3;++c)if(std::abs(value.rotation[c]-(*previous_rotation)[c])>180)
                    return invalid("Bake crosses an Euler-angle wrap; retain the procedural animation or shorten its interval");
                previous_rotation=value.rotation;
            }
            insert(t,output.property=="position"?value.position:value.rotation,
                i?vng::timeline::Interpolation::linear:vng::timeline::Interpolation::hold);
        }
        if(a.interval.last<state.document.timeline_duration) {
            const auto t=std::nextafter(a.interval.last,state.document.timeline_duration);insert(t,raw(t),vng::timeline::Interpolation::hold);
            // Existing arriving interpolation resumes from the restored sample.
        }
        std::ranges::sort(track.keys,{},&vng::timeline::Keyframe::time);
        if(auto result=timeline.replace_track(std::move(track));!result)return invalid(result.error().message);
    }
    if(auto valid=validate_animation(animation_properties(state),timeline,state.document.timeline_duration,state.document.keyframe_names);!valid)
        return valid;
    state.document.timeline=std::move(timeline);
    return erase_instance(state,id);
}
std::string animation_debug_string(const AnimationSettings& a) {
    std::ostringstream out;out<<(a.enabled?"Enabled":"Disabled")<<" / ["<<a.interval.first<<", "<<a.interval.last<<"] s\n";
    if(const auto* d=std::get_if<DepartureSequence>(&a.root))
        out<<"Departure\n  Ship motion #"<<d->ship<<"\n    Route (cubic Bezier)\n    Speed profile\n    Turbulence\n  Camera follow #"<<d->camera<<" (consumes ship result)\n";
    else out<<"Spin #"<<std::get<SpinAnimation>(a.root).target<<"\n";
    for(const auto& target:animation_outputs(a))out<<"writes #"<<target.object<<" / "<<target.property<<"\n";
    out<<"Owned parameters: ";write_scene_animation(out,a);
    return out.str();
}
} // namespace editor_example
