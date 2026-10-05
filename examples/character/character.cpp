#include "character.hpp"
#include <vng/content/vmesh.hpp>
#include <vng/content/vmesh_schema.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace character {
namespace {
using namespace vng;

content::Diagnostic problem(std::string message, const std::filesystem::path& file) {
    content::Diagnostic error;
    error.code = content::ErrorCode::invalid_document;
    error.message = std::move(message);
    error.path = file;
    return error;
}
content::Diagnostic from_rig(const rig::Diagnostic& error, const std::filesystem::path& file) {
    return problem(error.vertex ? std::format("vertex {}: {}", *error.vertex, error.message) : error.message, file);
}
const content::vmesh::VertexField* field(const content::vmesh::Document& document, std::string_view name) {
    const auto found = std::ranges::find(document.vertex_fields, name, &content::vmesh::VertexField::name);
    return found == document.vertex_fields.end() ? nullptr : &*found;
}
} // namespace

rig::Transform blend(const rig::Transform& a, const rig::Transform& b, f32 t) {
    const auto mix = [t](f32 x, f32 y) { return x + (y - x) * t; };
    auto q = b.rotation;
    if (a.rotation.x * q.x + a.rotation.y * q.y + a.rotation.z * q.z + a.rotation.w * q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
    rig::Quat r{mix(a.rotation.x, q.x), mix(a.rotation.y, q.y), mix(a.rotation.z, q.z), mix(a.rotation.w, q.w)};
    const auto norm = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    return {{mix(a.translation.x, b.translation.x), mix(a.translation.y, b.translation.y), mix(a.translation.z, b.translation.z)},
            {r.x / norm, r.y / norm, r.z / norm, r.w / norm}, mix(a.scale, b.scale)};
}

content::Result<Character> Character::load(const std::filesystem::path& mesh_path) {
    auto document = content::vmesh::read_vmesh(mesh_path);
    if (!document) return std::unexpected(document.error());
    const auto rig_name = document->metadata.find("skin/rig");
    if (rig_name == document->metadata.end())
        return std::unexpected(problem("The mesh names no rig (skin/rig metadata)", mesh_path));
    auto rig_file = read_rig(mesh_path.parent_path() / rig_name->second);
    if (!rig_file) return std::unexpected(rig_file.error());

    rig::ArmatureBuilder builder;
    std::vector<rig::BoneId> bones;
    for (const auto& bone : rig_file->bones) {
        std::optional<rig::BoneId> parent;
        if (!bone.parent.empty())
            parent = bones[static_cast<std::size_t>(std::ranges::find(rig_file->bones, bone.parent, &Bone::name) - rig_file->bones.begin())];
        bones.push_back(builder.add_bone(bone.name, parent, bone.rest));
    }
    auto armature = builder.build();
    if (!armature) return std::unexpected(from_rig(armature.error(), mesh_path));

    auto schema = content::vmesh::schema<Vertex>();
    schema.map("position", gfx::Position{});
    schema.map("normal", gfx::Normal{});
    schema.map("color/0", gfx::Color{});
    auto mesh = content::vmesh::decode(*document, schema);
    if (!mesh) return std::unexpected(mesh.error());
    auto binding = rig::bind(std::move(*mesh), *armature);
    if (!binding) return std::unexpected(from_rig(binding.error(), mesh_path));

    // Influences come in groups of four: skin/bones/N (u32x4) with skin/weights/N (f32x4).
    std::vector<std::vector<rig::Influence>> influences(document->vertex_count);
    for (unsigned group = 0;; ++group) {
        const auto* ids = field(*document, std::format("skin/bones/{}", group));
        const auto* weights = field(*document, std::format("skin/weights/{}", group));
        if (!ids && !weights) break;
        if (!ids || !weights || ids->type != content::vmesh::FieldType{content::vmesh::ScalarType::UInt32, 4} ||
            weights->type != content::vmesh::FieldType{content::vmesh::ScalarType::Float32, 4})
            return std::unexpected(problem(std::format("skin/bones/{0} must be u32x4 beside skin/weights/{0} as f32x4", group), mesh_path));
        const auto& id_values = std::get<std::vector<u32>>(ids->values);
        const auto& weight_values = std::get<std::vector<f32>>(weights->values);
        for (std::size_t v = 0; v < document->vertex_count; ++v)
            for (std::size_t k = 0; k < 4; ++k) {
                const auto weight = weight_values[v * 4 + k];
                if (weight == 0) continue;
                const auto bone = id_values[v * 4 + k];
                if (bone >= bones.size() || !(weight > 0) || !std::isfinite(weight))
                    return std::unexpected(problem(std::format("vertex {}: bad skin influence", v), mesh_path));
                influences[v].push_back({bones[bone], weight});
            }
    }
    for (std::size_t v = 0; v < influences.size(); ++v)
        if (auto set = binding->set_weights(v, influences[v]); !set) return std::unexpected(from_rig(set.error(), mesh_path));
    if (auto normalized = binding->normalize_weights(); !normalized) return std::unexpected(from_rig(normalized.error(), mesh_path));
    return Character{std::move(*rig_file), std::move(*armature), std::move(bones), std::move(*binding)};
}

Character::Character(RigFile rig, rig::Armature armature, std::vector<rig::BoneId> bones, Binding binding)
    : rig_(std::move(rig)), armature_(std::move(armature)), bones_(std::move(bones)), binding_(std::move(binding)) {
    const auto& mesh = binding_.mesh();
    for (const auto& light : rig_.lights) {
        std::size_t nearest{};
        f32 best = std::numeric_limits<f32>::max();
        for (std::size_t v = 0; v < mesh.vertex_count(); ++v) {
            const auto p = mesh.vertices()[v].get(gfx::Position{});
            const auto d = (p.x - light.position.x) * (p.x - light.position.x) + (p.y - light.position.y) * (p.y - light.position.y) +
                           (p.z - light.position.z) * (p.z - light.position.z);
            if (d < best) { best = d; nearest = v; }
        }
        light_vertices_.push_back(nearest);
    }
}

std::vector<render::SpotLight> Character::spots(const rig::Pose& pose) const {
    std::vector<render::SpotLight> out;
    if (rig_.lights.empty()) return out;
    const auto palette = binding_.palette(pose);
    if (!palette) return out;
    for (std::size_t k = 0; k < rig_.lights.size(); ++k) {
        const auto& light = rig_.lights[k];
        const auto influences = binding_.weights().influences(light_vertices_[k]);
        if (!influences) continue;
        Mat4 m{};                                   // the skin's blend of bone matrices there
        for (const auto& influence : *influences)
            if (const auto index = armature_.index(influence.bone); index && *index < palette->size())
                for (std::size_t c = 0; c < 4; ++c) {
                    const auto& column = (*palette)[*index][c];
                    m[c] = Vec4{m[c].x + column.x * influence.weight, m[c].y + column.y * influence.weight,
                                m[c].z + column.z * influence.weight, m[c].w + column.w * influence.weight};
                }
        const auto& p = light.position;
        const auto& d = light.direction;
        const Vec3 at{m[0].x * p.x + m[1].x * p.y + m[2].x * p.z + m[3].x, m[0].y * p.x + m[1].y * p.y + m[2].y * p.z + m[3].y,
                      m[0].z * p.x + m[1].z * p.y + m[2].z * p.z + m[3].z};
        const Vec3 toward{m[0].x * d.x + m[1].x * d.y + m[2].x * d.z, m[0].y * d.x + m[1].y * d.y + m[2].y * d.z,
                          m[0].z * d.x + m[1].z * d.y + m[2].z * d.z};
        out.push_back({.position = at, .direction = toward, .color = light.color, .inner = light.inner, .outer = light.outer, .range = light.range});
    }
    return out;
}

content::Result<void> Character::pose(const Clip& clip, f32 seconds, rig::Pose& pose) const {
    const auto frames = static_cast<f32>(clip.frames);
    auto at = seconds * clip.fps;
    if (clip.loop) at = std::fmod(std::fmod(at, frames) + frames, frames);
    else at = std::clamp(at, 0.F, frames - 1);
    const auto first = static_cast<std::size_t>(at);
    const auto next = clip.loop ? (first + 1) % clip.frames : std::min(first + 1, clip.frames - 1);
    const auto t = at - static_cast<f32>(first);
    for (const auto& track : clip.tracks)
        if (auto set = pose.set_local(bones_[track.bone], blend(track.frames[first], track.frames[next], t)); !set)
            return std::unexpected(from_rig(set.error(), {}));
    return {};
}
} // namespace character
