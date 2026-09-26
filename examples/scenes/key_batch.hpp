#pragma once
#include "../editor/animation.hpp"
#include <map>
#include <utility>

namespace example {
// Many timeline keys, one validated transaction. key_property copies and
// revalidates the whole timeline per call, which is quadratic for generators
// that bake thousands of samples. Commit follows key_property's rules: a
// track whose first key is after zero keeps its base value at zero, and
// boolean tracks hold. Successive linear Euler rotation keys are wrapped to
// take the short way round, since rotations interpolate per component.
class KeyBatch final {
public:
    void key(vng::timeline::Target, vng::f32 time, vng::timeline::Value,
             vng::timeline::Interpolation incoming = vng::timeline::Interpolation::linear);
    // Drops pending samples of one f32/Vec3 track that a straight line between
    // the kept neighbors already reproduces within `tolerance`. Cuts (hold
    // keys) and the keys just before them are always kept.
    void simplify(const vng::timeline::Target&, vng::f32 tolerance);
    [[nodiscard]] vng::content::Result<void> commit(editor_example::State&);
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    std::map<std::pair<vng::u64, std::string>, std::map<vng::f32, vng::timeline::Keyframe>> tracks_;
    std::size_t count_{};
};

// A shot framed by an eye and a look target. Keying the camera instance's eye
// directly keeps it exact; the focus (orbit pivot) is the subject distance.
struct Look {
    vng::Vec3 eye, target;
    vng::f32 zoom{1};
};
void key_look(KeyBatch&, vng::u32 camera, vng::f32 time, const Look&,
              vng::timeline::Interpolation incoming = vng::timeline::Interpolation::linear);
// The same look as an orbit pose around its target, e.g. for an editor view.
[[nodiscard]] editor_example::CameraPose orbit_pose(const Look&);
// Instance Euler rotation (degrees, T*Rz*Ry*Rx) that points local -Z along
// `forward` with local +Y as close to `up` as possible.
[[nodiscard]] vng::Vec3 orientation(vng::Vec3 forward, vng::Vec3 up);
} // namespace example
