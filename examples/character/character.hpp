#pragma once
#include "character_file.hpp"
#include <vng/gfx/geometry.hpp>
#include <vng/rig/rig.hpp>
#include <vng/rig/skinned_mesh_renderer.hpp>

namespace character {
using Vertex = vng::gfx::Record<vng::gfx::Position, vng::gfx::Normal, vng::gfx::Color>;
using Mesh = vng::gfx::Mesh<Vertex>;
using Binding = vng::rig::SkinBinding<Mesh>;

// A skinned character: its mesh bound to its armature, and the clips that
// animate it. The mesh stays in bind pose; poses move it only on the GPU.
class Character {
public:
    // Reads the skinned mesh and the .vrig its `skin/rig` metadata names
    // (relative to the mesh). Skin weights come from its skin/bones/N and
    // skin/weights/N fields and are normalized per vertex.
    [[nodiscard]] static vng::content::Result<Character> load(const std::filesystem::path& mesh);

    [[nodiscard]] const RigFile& rig() const { return rig_; }
    [[nodiscard]] const vng::rig::Armature& armature() const { return armature_; }
    [[nodiscard]] const Binding& binding() const { return binding_; }
    [[nodiscard]] const Clip* clip(std::string_view name) const { return rig_.clip(name); }
    [[nodiscard]] vng::rig::BoneId bone(std::size_t index) const { return bones_.at(index); }
    // Poses every bone the clip animates at `seconds`: a looping clip wraps
    // and blends its last frame into its first, any other holds its ends.
    [[nodiscard]] vng::content::Result<void> pose(const Clip&, vng::f32 seconds, vng::rig::Pose&) const;
    // The lights it carries (RigFile::lights), where a pose puts them, for a
    // skinned draw at the mesh's own placement. A light moves as the skin
    // nearest it does, so it stays on (and aims along) what carries it.
    [[nodiscard]] std::vector<vng::render::SpotLight> spots(const vng::rig::Pose&) const;

private:
    Character(RigFile rig, vng::rig::Armature armature, std::vector<vng::rig::BoneId> bones, Binding binding);
    RigFile rig_;
    vng::rig::Armature armature_;
    std::vector<vng::rig::BoneId> bones_; // in the rig file's order
    Binding binding_;
    std::vector<std::size_t> light_vertices_; // per light, the bind-pose vertex nearest it
};

// Frame blend of two transforms: linear translation and scale, shortest-path
// normalized rotation.
[[nodiscard]] vng::rig::Transform blend(const vng::rig::Transform& a, const vng::rig::Transform& b, vng::f32 t);
} // namespace character
