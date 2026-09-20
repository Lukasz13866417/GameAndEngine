#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include <vng/core/types.hpp>
#include <vng/resources/diagnostic.hpp>

namespace example::sun {

struct Options final {
    vng::Extent2D extent{1280, 800};
    std::optional<vng::u64> frame_limit;
    std::optional<vng::f32> fixed_time;
    std::optional<std::filesystem::path> analysis_directory;
    std::optional<std::filesystem::path> screenshot_path;
    bool bloom{true};
    bool displacement{true};
    bool white_spots{false};
    bool help{};
};

// Normal invocation animates until the window closes. Capture flags default to
// one frame at t=3 seconds; explicit --time / --frames settings take precedence.
[[nodiscard]] vng::resources::Result<Options> parse_options(int argc, char** argv);
[[nodiscard]] std::string_view usage() noexcept;

} // namespace example::sun
