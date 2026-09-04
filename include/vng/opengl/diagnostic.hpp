#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vng::opengl {

enum class ErrorCode {
    invalid_context_access,
    context_expired,
    context_not_current,
    wrong_thread,
    procedure_loading_failed,
    unsupported_version,
    unsupported_feature,
    invalid_argument,
    incompatible_device,
    shader_creation_failed,
    shader_compilation_failed,
    program_creation_failed,
    program_link_failed,
    object_creation_failed,
    framebuffer_incomplete,
    operation_failed,
};

struct SourceMapEntry final {
    std::uint32_t generated_line{};
    std::uint32_t ir_node{};
};

struct Diagnostic final {
    ErrorCode code{ErrorCode::operation_failed};
    std::string message{};
    std::string driver_log{};
    std::string generated_source{};
    std::vector<SourceMapEntry> source_map{};
};

enum class DebugSeverity { notification, low, medium, high, unknown };

struct DebugMessage final {
    std::uint32_t id{};
    DebugSeverity severity{DebugSeverity::unknown};
    std::string source{};
    std::string type{};
    std::string message{};
};

} // namespace vng::opengl
