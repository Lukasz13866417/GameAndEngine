#include "sun_options.hpp"

#include <charconv>
#include <cmath>
#include <string>
#include <utility>

namespace example::sun {
namespace {

vng::resources::Diagnostic option_error(std::string message)
{
    vng::resources::Diagnostic error;
    error.code = vng::resources::ErrorCode::invalid_argument;
    error.message = std::move(message);
    error.context.emplace_back(usage());
    return error;
}

template<class T>
bool number(std::string_view text, T& result)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

} // namespace

std::string_view usage() noexcept
{
    return "vng_sun_demo [--once | --frames N] [--time SECONDS] "
        "[--screenshot IMAGE.png] [--analyze NEW_DIRECTORY] "
        "[--no-bloom] [--no-displacement] [--white-spots | --no-white-spots] "
        "[--width PIXELS] [--height PIXELS] [--help]\n"
        "Without capture flags, the sun animates until the window closes.\n"
        "Space: pause | B: bloom | D: displacement | W: white spots | R: reset | Esc: close\n"
        "Captures default to one frame at 3 seconds; explicit time/frame limits override this.\n";
}

vng::resources::Result<Options> parse_options(int argc, char** argv)
{
    Options options;
    if (argc < 1 || !argv || !argv[0]) {
        return std::unexpected(option_error("Invalid argument list"));
    }
    bool width_set = false, height_set = false;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) return std::unexpected(option_error("Invalid null argument"));
        const std::string_view argument{argv[i]};
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--once") {
            if (options.frame_limit) {
                return std::unexpected(option_error("Specify only one frame limit"));
            }
            options.frame_limit = 1;
        } else if (argument == "--no-bloom") {
            options.bloom = false;
        } else if (argument == "--no-displacement") {
            options.displacement = false;
        } else if (argument == "--no-white-spots") {
            options.white_spots = false;
        } else if (argument == "--white-spots") {
            options.white_spots = true;
        } else if (argument == "--frames" || argument == "--time"
            || argument == "--analyze" || argument == "--screenshot"
            || argument == "--width" || argument == "--height") {
            if (i + 1 >= argc || !argv[i + 1]) {
                return std::unexpected(option_error(std::string(argument) + " requires a value"));
            }
            const std::string_view value{argv[++i]};
            if (argument == "--frames") {
                vng::u64 count{};
                if (options.frame_limit || !number(value, count) || count == 0) {
                    return std::unexpected(option_error("--frames requires one positive integer"));
                }
                options.frame_limit = count;
            } else if (argument == "--time") {
                vng::f32 seconds{};
                if (options.fixed_time || !number(value, seconds)
                    || !std::isfinite(seconds) || seconds < 0) {
                    return std::unexpected(option_error("--time requires one finite nonnegative value in seconds"));
                }
                options.fixed_time = seconds;
            } else if (argument == "--width" || argument == "--height") {
                auto& already_set = argument == "--width" ? width_set : height_set;
                vng::u32 pixels{};
                if (already_set || !number(value, pixels) || pixels < 64 || pixels > 8192) {
                    return std::unexpected(option_error(std::string(argument)
                        + " requires one integer in [64, 8192]"));
                }
                auto& dimension = argument == "--width" ? options.extent.width : options.extent.height;
                dimension = pixels;
                already_set = true;
            } else {
                auto& destination = argument == "--analyze"
                    ? options.analysis_directory : options.screenshot_path;
                if (destination || value.empty() || value.starts_with('-')) {
                    return std::unexpected(option_error(std::string(argument)
                        + " requires one nonempty output path"));
                }
                destination = std::filesystem::path(value);
            }
        } else {
            return std::unexpected(option_error("Unrecognized argument: " + std::string(argument)));
        }
    }

    if (options.analysis_directory || options.screenshot_path) {
        if (!options.fixed_time) options.fixed_time = 3;
        if (!options.frame_limit) options.frame_limit = 1;
    }
    return options;
}

} // namespace example::sun
