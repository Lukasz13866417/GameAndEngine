#include "character_file.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace character {
namespace {
using namespace vng;
using content::Reader;

content::Diagnostic invalid(std::string message, const std::filesystem::path& file = {}) {
    content::Diagnostic error;
    error.code = content::ErrorCode::invalid_document;
    error.message = std::move(message);
    error.path = file;
    return error;
}
[[noreturn]] void fail_at(const Reader& reader, std::string_view key, std::string message) {
    if (reader.view().child(key)) reader.child(key).fail(std::move(message));
    reader.fail(std::move(message));
}
// Files hold quaternions rounded to f32 text; renormalize those that are unit
// within that rounding, and reject anything else as a mistake.
bool make_transform(Vec3 t, std::array<f32, 4> q, f32 s, rig::Transform& out) {
    const auto norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!std::isfinite(norm) || std::abs(norm - 1.F) > 1e-3F) return false;
    out = {t, {q[0] / norm, q[1] / norm, q[2] / norm, q[3] / norm}, s};
    return rig::validate(out).has_value();
}
} // namespace

const Clip* RigFile::clip(std::string_view name) const {
    const auto found = std::ranges::find(clips, name, &Clip::name);
    return found == clips.end() ? nullptr : &*found;
}

content::Result<RigFile> parse_rig(std::string_view source, const std::filesystem::path& file) {
    auto document = content::parse_document(source, {.limits = {.max_source_bytes = 64U << 20,
        .max_decoded_bytes = 256U << 20, .max_depth = 16, .max_string_bytes = 64U << 10}, .source_path = file});
    if (!document) return std::unexpected(document.error());
    if (document->kind() != "vrig") return std::unexpected(invalid("Not a rig document (expected \"vrig 1.0\")", file));
    return document->read([&](Reader& r) {
        if (r.get<u32>("rig") != 1) fail_at(r, "rig", "Unsupported rig version");
        RigFile result;
        result.name = r.get_or<std::string>("name", "");
        result.source = r.get_or<std::string>("source", "");
        std::map<std::string, std::size_t, std::less<>> index;
        for (const auto entry : r.child("bones").elements()) {
            Bone bone;
            bone.name = entry.get<std::string>("name");
            bone.parent = entry.get_or<std::string>("parent", "");
            if (bone.name.empty() || index.contains(bone.name)) fail_at(entry, "name", "Bone names must be unique and nonempty");
            if (!bone.parent.empty() && !index.contains(bone.parent))
                fail_at(entry, "parent", "A bone's parent must be listed before it");
            if (!make_transform(entry.get<Vec3>("translation"), entry.get<std::array<f32, 4>>("rotation"),
                                entry.get_or<f32>("scale", 1), bone.rest))
                entry.fail("A rest transform needs finite values, a unit rotation and a positive scale");
            if (result.bones.size() == max_bones) entry.fail("Too many bones");
            index.emplace(bone.name, result.bones.size());
            result.bones.push_back(std::move(bone));
        }
        if (result.bones.empty()) fail_at(r, "bones", "A rig needs at least one bone");
        if (r.view().child("clips"))
            for (const auto entry : r.child("clips").elements()) {
                Clip clip;
                clip.name = entry.get<std::string>("name");
                clip.fps = entry.get<f32>("fps");
                clip.frames = entry.get<u32>("frames");
                clip.loop = entry.get_or<bool>("loop", true);
                clip.speed = entry.get_or<f32>("speed", 0);
                if (clip.name.empty() || result.clip(clip.name)) fail_at(entry, "name", "Clip names must be unique and nonempty");
                if (!(clip.fps > 0 && std::isfinite(clip.fps))) fail_at(entry, "fps", "fps must be positive");
                if (clip.frames == 0 || clip.frames > max_frames) fail_at(entry, "frames", "A clip needs 1 to 100000 frames");
                if (!std::isfinite(clip.speed)) fail_at(entry, "speed", "speed must be finite");
                std::vector<bool> tracked(result.bones.size());
                for (const auto t : entry.child("tracks").elements()) {
                    const auto bone = index.find(t.get<std::string>("bone"));
                    if (bone == index.end()) fail_at(t, "bone", "The track names no bone");
                    if (tracked[bone->second]) fail_at(t, "bone", "A bone has two tracks in one clip");
                    tracked[bone->second] = true;
                    const auto translation = t.get<std::vector<f32>>("translation");
                    const auto rotation = t.get<std::vector<f32>>("rotation");
                    const auto scale = t.get<std::vector<f32>>("scale");
                    if (translation.size() != clip.frames * 3 || rotation.size() != clip.frames * 4 || scale.size() != clip.frames)
                        t.fail("A track needs 3, 4 and 1 values per frame for translation, rotation and scale");
                    Track track{bone->second, std::vector<rig::Transform>(clip.frames)};
                    for (std::size_t f = 0; f < clip.frames; ++f)
                        if (!make_transform({translation[f * 3], translation[f * 3 + 1], translation[f * 3 + 2]},
                                            {rotation[f * 4], rotation[f * 4 + 1], rotation[f * 4 + 2], rotation[f * 4 + 3]},
                                            scale[f], track.frames[f]))
                            t.fail("Frame " + std::to_string(f) + " needs finite values, a unit rotation and a positive scale");
                    clip.tracks.push_back(std::move(track));
                }
                if (result.clips.size() == max_clips) entry.fail("Too many clips");
                result.clips.push_back(std::move(clip));
            }
        if (r.view().child("lights"))
            for (const auto entry : r.child("lights").elements()) {
                Light light;
                light.name = entry.get<std::string>("name");
                light.position = entry.get<Vec3>("position");
                light.direction = entry.get<Vec3>("direction");
                light.color = entry.get_or<Vec3>("color", Vec3{1, 1, 1});
                light.inner = entry.get_or<f32>("inner", 12);
                light.outer = entry.get_or<f32>("outer", 20);
                light.range = entry.get_or<f32>("range", 8);
                const auto finite = [](Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
                const auto& d = light.direction;
                if (!finite(light.position) || !finite(d) || !finite(light.color) || !(d.x * d.x + d.y * d.y + d.z * d.z > 1e-12F))
                    entry.fail("A light needs a finite position, colour and nonzero direction");
                if (!(light.inner >= 0 && light.inner <= light.outer && light.outer < 90) || !(light.range > 0 && std::isfinite(light.range)))
                    entry.fail("A light needs 0 <= inner <= outer < 90 degrees and a positive range");
                if (result.lights.size() == max_lights) entry.fail("Too many lights");
                result.lights.push_back(std::move(light));
            }
        return result;
    });
}

content::Result<RigFile> read_rig(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        auto error = invalid("Cannot open the rig", file);
        error.code = content::ErrorCode::io_error;
        return std::unexpected(std::move(error));
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return parse_rig(std::move(buffer).str(), file);
}
} // namespace character
