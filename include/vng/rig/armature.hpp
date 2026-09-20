#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vng/rig/math.hpp>

namespace vng::rig {

class ArmatureBuilder;
class Armature;
class Pose;
class SkinWeights;

// Identity includes the owning immutable armature. A bone from a different
// armature cannot accidentally alias a bone with the same dense array index.
class BoneId final {
public:
    constexpr BoneId() noexcept = default;
    [[nodiscard]] explicit constexpr operator bool() const noexcept { return owner_ != 0; }
    friend constexpr bool operator==(BoneId, BoneId) = default;
private:
    constexpr BoneId(u64 owner, u32 index) noexcept : owner_(owner), index_(index) {}
    u64 owner_{};
    u32 index_{};
    friend class ArmatureBuilder;
    friend class Armature;
    friend class Pose;
    friend class SkinWeights;
};

struct Bone final {
    BoneId id;
    std::string name;
    std::optional<BoneId> parent;
    Transform rest_local;
};

class Armature final {
public:
    Armature() = default;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    [[nodiscard]] u64 identity() const noexcept;
    [[nodiscard]] std::size_t bone_count() const noexcept;
    [[nodiscard]] std::span<const Bone> bones() const noexcept;
    [[nodiscard]] std::expected<BoneId, Diagnostic> bone(std::string_view name) const;
    [[nodiscard]] std::expected<u32, Diagnostic> index(BoneId bone) const;
    [[nodiscard]] Pose rest_pose() const;
    [[nodiscard]] std::span<const Mat4> rest_globals() const noexcept;
private:
    struct Data;
    explicit Armature(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
    std::shared_ptr<const Data> data_;
    friend class ArmatureBuilder;
};

class ArmatureBuilder final {
public:
    ArmatureBuilder();
    ArmatureBuilder(const ArmatureBuilder&) = delete;
    ArmatureBuilder& operator=(const ArmatureBuilder&) = delete;
    // Parents must already have been added. This guarantees topological order
    // and makes cycles unrepresentable. Errors are reported together at build().
    [[nodiscard]] BoneId add_bone(
        std::string name, std::optional<BoneId> parent = {}, Transform rest = {});
    [[nodiscard]] std::expected<Armature, Diagnostic> build();
private:
    u64 identity_;
    std::vector<Bone> bones_;
    std::optional<Diagnostic> error_;
    std::optional<Armature> built_;
};

class Pose final {
public:
    Pose() = default;
    [[nodiscard]] const Armature& armature() const noexcept { return armature_; }
    [[nodiscard]] u64 revision() const noexcept { return revision_; }
    [[nodiscard]] std::expected<Transform, Diagnostic> local(BoneId bone) const;
    [[nodiscard]] std::expected<void, Diagnostic> set_local(BoneId bone, Transform transform);
    // Absolute parent-relative rotation, not an extra rotation from rest.
    [[nodiscard]] std::expected<void, Diagnostic> set_local_rotation(BoneId bone, Quat rotation);
    void reset();
    [[nodiscard]] std::expected<std::vector<Mat4>, Diagnostic> globals() const;
private:
    explicit Pose(Armature armature);
    Armature armature_;
    std::vector<Transform> locals_;
    u64 revision_{};
    friend class Armature;
};

} // namespace vng::rig
