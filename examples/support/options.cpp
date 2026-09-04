#include "options.hpp"

#include <charconv>
#include <string_view>
#include <utility>

namespace example {
namespace {

[[nodiscard]] bool parse_positive_integer(
    std::string_view text,
    std::uint64_t& result)
{
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{}
        && parsed.ptr == text.data() + text.size()
        && result > 0;
}

[[nodiscard]] UsageError frame_usage(const char* executable)
{
    return {"usage: " + std::string(executable)
        + " [--once | --frames N]"};
}

[[nodiscard]] UsageError mesh_usage(const char* executable)
{
    return {"usage: " + std::string(executable)
        + " [mesh.vmesh] [--once | --frames N] [--analyze]"};
}

} // namespace

std::expected<FrameOptions, UsageError>
parse_frame_options(int argc, char** argv)
{
    FrameOptions options;
    if (argc == 1) {
        return options;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--once") {
        options.frame_limit = 1;
        return options;
    }
    if (argc != 3 || std::string_view(argv[1]) != "--frames") {
        return std::unexpected(frame_usage(argv[0]));
    }

    std::uint64_t count{};
    if (!parse_positive_integer(argv[2], count)) {
        return std::unexpected(frame_usage(argv[0]));
    }
    options.frame_limit = count;
    return options;
}

std::expected<MeshOptions, UsageError> parse_mesh_options(
    int argc,
    char** argv,
    std::filesystem::path default_mesh)
{
    MeshOptions options{
        .mesh_path = std::move(default_mesh),
        .frame_limit = std::nullopt,
        .analyze = false,
    };
    bool custom_path = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--once") {
            if (options.frame_limit) {
                return std::unexpected(mesh_usage(argv[0]));
            }
            options.frame_limit = 1;
        } else if (argument == "--frames") {
            if (options.frame_limit || index + 1 >= argc) {
                return std::unexpected(mesh_usage(argv[0]));
            }
            std::uint64_t count{};
            if (!parse_positive_integer(argv[++index], count)) {
                return std::unexpected(mesh_usage(argv[0]));
            }
            options.frame_limit = count;
        } else if (argument == "--analyze") {
            if (options.analyze) {
                return std::unexpected(mesh_usage(argv[0]));
            }
            options.analyze = true;
        } else {
            if (argument.starts_with('-') || custom_path) {
                return std::unexpected(mesh_usage(argv[0]));
            }
            options.mesh_path = argument;
            custom_path = true;
        }
    }
    return options;
}

} // namespace example
