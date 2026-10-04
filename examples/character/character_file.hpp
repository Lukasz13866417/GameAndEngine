#pragma once
#include <vng/content/document.hpp>
#include <vng/rig/math.hpp>
#include <filesystem>
#include <string>
#include <vector>

// A character's armature and animation clips, stored as a "vrig 1.0"
// document beside the skinned .vmesh whose `skin/rig` metadata names it.
// examples/tools/import_model.py writes both; docs/characters.md describes
// the format. Bones are listed parents first; transforms are parent-relative
// with a unit quaternion (x, y, z, w) and a positive uniform scale.
namespace character {
struct Bone {
    std::string name;
    std::string parent; // empty for a root
    vng::rig::Transform rest;
};
// One bone's local transform at every frame of a clip.
struct Track {
    std::size_t bone{}; // index into RigFile::bones
    std::vector<vng::rig::Transform> frames;
};
struct Clip {
    std::string name;
    vng::f32 fps{24};
    std::size_t frames{};
    bool loop{true};
    vng::f32 speed{}; // forward (+Z) metres per second that keep the feet planted
    std::vector<Track> tracks;
    // A looping clip's last frame blends back into its first.
    [[nodiscard]] vng::f32 duration() const { return static_cast<vng::f32>(loop ? frames : frames - 1) / fps; }
};
// A light the character carries, such as a flashlight: where it is and where
// it points in the bind pose (it rides on the skin there), its colour
// (linear, times its strength), the inner and outer edges of its cone in
// degrees off its axis, and the metres at which it has faded out.
struct Light {
    std::string name;
    vng::Vec3 position{}, direction{0, 0, 1}, color{1, 1, 1};
    vng::f32 inner{12}, outer{20}, range{8};
};
struct RigFile {
    std::string name, source;
    std::vector<Bone> bones;
    std::vector<Clip> clips;
    std::vector<Light> lights;
    [[nodiscard]] const Clip* clip(std::string_view name) const;
};
inline constexpr std::size_t max_bones = 1024, max_clips = 256, max_frames = 100'000, max_lights = 16;

[[nodiscard]] vng::content::Result<RigFile> parse_rig(std::string_view source, const std::filesystem::path& file);
[[nodiscard]] vng::content::Result<RigFile> read_rig(const std::filesystem::path& file);
} // namespace character
