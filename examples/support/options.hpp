#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace example {

struct UsageError final {
    std::string message;
};

struct FrameOptions final {
    std::optional<std::uint64_t> frame_limit;
};

struct MeshOptions final {
    std::filesystem::path mesh_path;
    std::optional<std::uint64_t> frame_limit;
    bool analyze{};
};

[[nodiscard]] std::expected<FrameOptions, UsageError>
parse_frame_options(int argc, char** argv);

[[nodiscard]] std::expected<MeshOptions, UsageError> parse_mesh_options(
    int argc,
    char** argv,
    std::filesystem::path default_mesh);

} // namespace example
