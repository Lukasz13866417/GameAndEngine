#pragma once

#include <charconv>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vng/core/types.hpp>

namespace example {
struct UiOptions {
    std::optional<vng::u64> frames;
    std::optional<std::filesystem::path> screenshot;
    bool help{};
};
inline constexpr std::string_view ui_help =
    "vng_ui_demo [--once | --frames N] [--screenshot NEW.png]\n";
inline std::expected<UiOptions, std::string> parse_ui_options(int argc, char** argv) {
    UiOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help")
            options.help = true;
        else if (arg == "--once")
            options.frames = 1;
        else if (arg == "--screenshot" && i + 1 < argc)
            options.screenshot = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            vng::u64 count{};
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), count);
            if (error != std::errc{} || end != value.data() + value.size() || count == 0)
                return std::unexpected("--frames requires a positive integer");
            options.frames = count;
        } else
            return std::unexpected("Unknown or incomplete UI demo argument; use --help");
    }
    if (options.screenshot && !options.frames)
        options.frames = 1;
    return options;
}
} // namespace example
