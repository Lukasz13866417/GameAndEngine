#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace vng::rig {

enum class ErrorCode {
    invalid_transform,
    invalid_armature,
    invalid_bone,
    duplicate_name,
    builder_finished,
    incompatible_pose,
    invalid_mesh,
    invalid_vertex,
    invalid_weight,
    incomplete_weights,
    unnormalized_weights,
    too_many_influences,
    invalid_influence_limit,
    invalid_matrix,
};

struct Diagnostic final {
    ErrorCode code{};
    std::string message;
    std::optional<std::size_t> vertex;
    friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

} // namespace vng::rig
