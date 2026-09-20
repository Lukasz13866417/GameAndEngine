#pragma once

#include "options.hpp"

#include <string_view>
#include <vector>

namespace example {

struct GlowOptions final {
    std::optional<std::uint64_t> frame_limit;
    bool bloom{true};
    bool reload{};
};

inline std::expected<GlowOptions, UsageError> parse_glow_options(
    int argc,
    char** argv)
{
    GlowOptions result;
    std::vector<char*> frames{argv[0]};

    // Remove glow-specific switches before applying the shared frame options.
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--no-bloom") {
            result.bloom = false;
        } else if (arg == "--reload") {
            result.reload = true;
        } else {
            frames.push_back(argv[i]);
        }
    }

    auto parsed = parse_frame_options(
        static_cast<int>(frames.size()),
        frames.data());
    if (!parsed) {
        return std::unexpected(UsageError{
            parsed.error().message + " [--no-bloom] [--reload]",
        });
    }

    result.frame_limit = parsed->frame_limit;
    return result;
}

} // namespace example
