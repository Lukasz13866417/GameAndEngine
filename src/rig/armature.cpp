#include <vng/rig/armature.hpp>

#include <algorithm>
#include <atomic>
#include <limits>

namespace vng::rig {
namespace {
std::atomic<u64> next_identity{1};
Diagnostic invalid_bone() { return {ErrorCode::invalid_bone, "bone does not belong to this armature", {}}; }
}

struct Armature::Data final {
    u64 identity;
    std::vector<Bone> bones;
    std::vector<Mat4> rest_globals;
};

bool Armature::valid() const noexcept { return data_ && !data_->bones.empty(); }
u64 Armature::identity() const noexcept { return data_ ? data_->identity : 0; }
std::size_t Armature::bone_count() const noexcept { return data_ ? data_->bones.size() : 0; }
std::span<const Bone> Armature::bones() const noexcept { return data_ ? std::span<const Bone>{data_->bones} : std::span<const Bone>{}; }
std::span<const Mat4> Armature::rest_globals() const noexcept { return data_ ? std::span<const Mat4>{data_->rest_globals} : std::span<const Mat4>{}; }

std::expected<BoneId, Diagnostic> Armature::bone(std::string_view name) const
{
    for (const auto& value : bones()) { if (value.name == name) { return value.id; } }
    return std::unexpected(Diagnostic{ErrorCode::invalid_bone, "no bone named '" + std::string(name) + "'", {}});
}

std::expected<u32, Diagnostic> Armature::index(BoneId bone) const
{
    if (!valid() || bone.owner_ != identity() || bone.index_ >= bone_count()) { return std::unexpected(invalid_bone()); }
    return bone.index_;
}

Pose Armature::rest_pose() const { return Pose{*this}; }

ArmatureBuilder::ArmatureBuilder() : identity_(next_identity.fetch_add(1, std::memory_order_relaxed)) {}

BoneId ArmatureBuilder::add_bone(std::string name, std::optional<BoneId> parent, Transform rest)
{
    if (error_) { return {}; }
    auto fail = [&](ErrorCode code, std::string message) {
        error_ = Diagnostic{code, std::move(message), {}};
        return BoneId{};
    };
    if (built_) { return fail(ErrorCode::builder_finished, "cannot add bones after building an immutable armature"); }
    if (bones_.size() >= std::numeric_limits<u32>::max()) { return fail(ErrorCode::invalid_armature, "too many bones"); }
    if (parent && (parent->owner_ != identity_ || parent->index_ >= bones_.size())) {
        return fail(ErrorCode::invalid_bone, "parent must be an earlier bone from this builder");
    }
    if (name.empty()) { return fail(ErrorCode::invalid_bone, "bone name cannot be empty"); }
    if (std::ranges::any_of(bones_, [&](const auto& value) { return value.name == name; })) {
        return fail(ErrorCode::duplicate_name, "duplicate bone name '" + name + "'");
    }
    if (auto result = validate(rest); !result) { error_ = result.error(); return {}; }
    const BoneId id{identity_, static_cast<u32>(bones_.size())};
    bones_.push_back(Bone{id, std::move(name), parent, rest});
    return id;
}

std::expected<Armature, Diagnostic> ArmatureBuilder::build()
{
    if (error_) { return std::unexpected(*error_); }
    if (built_) { return *built_; }
    if (bones_.empty()) { return std::unexpected(Diagnostic{ErrorCode::invalid_armature, "an armature needs at least one bone", {}}); }
    std::vector<Mat4> globals;
    globals.reserve(bones_.size());
    for (const auto& bone : bones_) {
        const auto local = matrix(bone.rest_local);
        const auto global = bone.parent ? multiply(globals[bone.parent->index_], local) : local;
        if (auto inverse = inverse_affine(global); !inverse) { return std::unexpected(inverse.error()); }
        globals.push_back(global);
    }
    built_ = Armature{std::make_shared<const Armature::Data>(Armature::Data{identity_, bones_, std::move(globals)})};
    return *built_;
}

Pose::Pose(Armature armature) : armature_(std::move(armature))
{
    locals_.reserve(armature_.bone_count());
    for (const auto& bone : armature_.bones()) { locals_.push_back(bone.rest_local); }
}

std::expected<Transform, Diagnostic> Pose::local(BoneId bone) const
{
    auto index = armature_.index(bone);
    if (!index) { return std::unexpected(index.error()); }
    return locals_[*index];
}

std::expected<void, Diagnostic> Pose::set_local(BoneId bone, Transform transform)
{
    auto index = armature_.index(bone);
    if (!index) { return std::unexpected(index.error()); }
    if (auto valid = validate(transform); !valid) { return valid; }
    if (locals_[*index] != transform) { locals_[*index] = transform; ++revision_; }
    return {};
}

std::expected<void, Diagnostic> Pose::set_local_rotation(BoneId bone, Quat rotation)
{
    auto transform = local(bone);
    if (!transform) { return std::unexpected(transform.error()); }
    transform->rotation = rotation;
    return set_local(bone, *transform);
}

void Pose::reset()
{
    bool changed = false;
    std::size_t index = 0;
    for (const auto& bone : armature_.bones()) {
        changed |= locals_[index] != bone.rest_local;
        locals_[index++] = bone.rest_local;
    }
    if (changed) { ++revision_; }
}

std::expected<std::vector<Mat4>, Diagnostic> Pose::globals() const
{
    if (!armature_) { return std::unexpected(Diagnostic{ErrorCode::invalid_armature, "pose has no armature", {}}); }
    std::vector<Mat4> result;
    result.reserve(locals_.size());
    for (std::size_t index = 0; index < locals_.size(); ++index) {
        const auto local = matrix(locals_[index]);
        const auto parent = armature_.bones()[index].parent;
        const auto global = parent ? multiply(result[parent->index_], local) : local;
        if (auto inverse = inverse_affine(global); !inverse) { return std::unexpected(inverse.error()); }
        result.push_back(global);
    }
    return result;
}

} // namespace vng::rig
