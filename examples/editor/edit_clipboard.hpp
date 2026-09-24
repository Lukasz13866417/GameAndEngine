#pragma once
#include "keyframes.hpp"

namespace editor_example {
// Session-local authoring clipboard. Instances reference shared blueprints;
// mesh bytes and GPU resources are never copied. Clear when loading a document.
class EditClipboard {
public:
    struct Pasted {
        std::optional<vng::u32> object; std::optional<vng::f32> keyframe;
        std::vector<vng::u32> objects; std::vector<vng::f32> keyframes;
    };
    void clear() { contents_=std::monostate{}; }
    [[nodiscard]] vng::content::Result<void> copy_instance(const State&, vng::u32);
    [[nodiscard]] vng::content::Result<void> copy_keyframe(const State&, vng::f32);
    [[nodiscard]] vng::content::Result<void> copy_instances(const State&, std::span<const vng::u32>);
    [[nodiscard]] vng::content::Result<void> copy_keyframes(const State&, std::span<const vng::f32>);
    [[nodiscard]] vng::content::Result<Pasted> paste(State&) const;
private:
    struct Instance { SceneInstance value; std::vector<vng::timeline::Track> tracks; };
    // `orbits`: whether each camera whose position was copied orbited then,
    // since that decides what its stored position means.
    struct Keyframe {
        std::string name; std::vector<KeyframeValue> values; vng::f32 offset{};
        std::vector<std::pair<vng::u32, bool>> orbits;
    };
    std::variant<std::monostate,std::vector<Instance>,std::vector<Keyframe>> contents_;
};
}
