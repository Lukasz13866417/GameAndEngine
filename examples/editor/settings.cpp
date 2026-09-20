#include "settings.hpp"
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <limits>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>

namespace editor_example {
namespace {
auto invalid(std::string message) {
    vng::content::Diagnostic error;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
}
vng::content::Result<void> validate_settings(const Settings& value) {
    for (auto speed : {value.camera_drag.pan, value.camera_drag.forward, value.camera_drag.rotation})
        if (!std::isfinite(speed) || speed < .001F || speed > 100.F)
            return invalid("Camera drag speeds must be 0.001..100.");
    if (value.ui_scale_percent < 75 || value.ui_scale_percent > 150)
        return invalid("UI scale must be 75..150 percent.");
    if (value.ui_fps > 240)
        return invalid("UI FPS cap must be 0..240 (0 = unlimited).");
    if (value.preview_fps < 1 || value.preview_fps > 240)
        return invalid("Embedded preview FPS cap must be 1..240.");
    if (value.play_fps > 240)
        return invalid("Independent Play FPS cap must be 0..240 (0 = unlimited).");
    if (value.preview_percent < 25 || value.preview_percent > 100)
        return invalid("Preview resolution must be 25..100% (100 = native).");
    if (!valid_orbit_distance_range(value.orbit_distance))
        return invalid("Orbit distance needs 0.001 <= minimum < maximum <= 1000000.");
    if (!valid_timeline_track_limit(value.timeline_track_limit))
        return invalid("Timeline track limit must be 1.." + std::to_string(vng::timeline::max_tracks) + ".");
    if (!valid_instance_limit(value.instance_limit))
        return invalid("Instance limit must be 1.." + std::to_string(max_scene_instances) + ".");
    if (value.vsync != vng::window::VSync::off && value.vsync != vng::window::VSync::on)
        return invalid("VSync must be off or on.");
    if (!std::isfinite(value.maximum_viewing_distance) || value.maximum_viewing_distance < 1 || value.maximum_viewing_distance > 10'000'000)
        return invalid("Maximum viewing distance must be 1..10000000 scene units.");
    for (float speed : {value.walk.forward,value.walk.sideways,value.walk.vertical})
        if (!std::isfinite(speed) || speed < .001F || speed > 1'000'000)
            return invalid("Walk speeds must be 0.001..1000000 scene units per second.");
    if (!std::isfinite(value.walk.fast_multiplier) || value.walk.fast_multiplier < 1 || value.walk.fast_multiplier > 100)
        return invalid("Walk Shift multiplier must be 1..100.");
    return {};
}
std::string encode_settings(const Settings& s) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10)
        << "vng-editor-settings 8\n" << s.ui_fps << ' ' << s.preview_fps << ' '
        << s.play_fps << ' ' << s.debug_fps << ' ' << s.preview_percent << '\n'
        << s.orbit_distance.minimum << ' ' << s.orbit_distance.maximum << '\n'
        << s.timeline_track_limit << '\n' << s.instance_limit << '\n'
        << (s.vsync == vng::window::VSync::on ? 1 : 0) << '\n'
        << s.maximum_viewing_distance << ' ' << s.walk.forward << ' ' << s.walk.sideways << ' '
        << s.walk.vertical << ' ' << s.walk.fast_multiplier << '\n' << s.ui_scale_percent << '\n'
        << s.camera_drag.pan << ' ' << s.camera_drag.forward << ' ' << s.camera_drag.rotation << ' '
        << (s.scroll_moves_camera ? 1 : 0) << '\n';
    return out.str();
}
vng::content::Result<Settings> decode_settings(std::string_view text) {
    constexpr std::string_view header = "vng-editor-settings 1\n";
    const bool version2 = text.starts_with("vng-editor-settings 2\n");
    const bool version3 = text.starts_with("vng-editor-settings 3\n");
    const bool version4 = text.starts_with("vng-editor-settings 4\n");
    const bool version6 = text.starts_with("vng-editor-settings 6\n");
    const bool version8 = text.starts_with("vng-editor-settings 8\n");
    const bool version7 = version8 || text.starts_with("vng-editor-settings 7\n");
    const bool version5 = text.starts_with("vng-editor-settings 5\n");
    if (text.size() > 256 || (!version7 && !version6 && !version5 && !version4 && !version3 && !version2 && !text.starts_with(header)))
        return invalid("Unknown or oversized editor settings document");
    text.remove_prefix(header.size());
    Settings result;
    for (auto* value : {&result.ui_fps, &result.preview_fps, &result.play_fps,
                       &result.debug_fps, &result.preview_percent}) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\r'))
            text.remove_prefix(1);
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *value);
        if (error != std::errc{} || end == text.data())
            return invalid("Editor settings require five whole numbers");
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        if (!text.empty() && text.front() != ' ' && text.front() != '\n' && text.front() != '\r')
            return invalid("Invalid editor settings separator");
    }
    if (version2 || version3 || version4 || version5 || version6 || version7) for (auto* value : {&result.orbit_distance.minimum, &result.orbit_distance.maximum}) {
        const auto first = text.find_first_not_of(" \r\n");
        if (first == std::string_view::npos) return invalid("Missing orbit-distance range");
        text.remove_prefix(first);
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *value);
        if (error != std::errc{} || end == text.data()) return invalid("Invalid orbit-distance range");
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        if (!text.empty() && text.front() != ' ' && text.front() != '\n' && text.front() != '\r')
            return invalid("Invalid orbit-distance range separator");
    }
    if (version3 || version4 || version5 || version6 || version7) {
        const auto first = text.find_first_not_of(" \r\n");
        if (first == std::string_view::npos) return invalid("Missing timeline track limit");
        text.remove_prefix(first);
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result.timeline_track_limit);
        if (error != std::errc{} || end == text.data()) return invalid("Invalid timeline track limit");
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    }
    if (version4 || version5 || version6 || version7) {
        if (text.empty() || (text.front() != ' ' && text.front() != '\n' && text.front() != '\r'))
            return invalid("Missing instance limit separator");
        const auto first = text.find_first_not_of(" \r\n");
        if (first == std::string_view::npos) return invalid("Missing instance limit");
        text.remove_prefix(first);
        const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), result.instance_limit);
        if (error != std::errc{} || end == text.data()) return invalid("Invalid instance limit");
        text.remove_prefix(static_cast<std::size_t>(end-text.data()));
    }
    if (version5 || version6 || version7) {
        if (text.empty() || (text.front() != ' ' && text.front() != '\n' && text.front() != '\r'))
            return invalid("Missing VSync separator");
        const auto first = text.find_first_not_of(" \r\n");
        if (first == std::string_view::npos) return invalid("Missing VSync preference");
        text.remove_prefix(first);
        unsigned mode{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), mode);
        if (error != std::errc{} || end == text.data() || mode > 1) return invalid("Invalid VSync preference");
        result.vsync = mode ? vng::window::VSync::on : vng::window::VSync::off;
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    }
    if (version6 || version7) for (auto* value : {&result.maximum_viewing_distance,&result.walk.forward,
            &result.walk.sideways,&result.walk.vertical,&result.walk.fast_multiplier}) {
        if (text.empty() || (text.front()!=' ' && text.front()!='\r' && text.front()!='\n'))
            return invalid("Missing camera preference separator");
        const auto first=text.find_first_not_of(" \r\n");
        if (first==std::string_view::npos) return invalid("Missing camera preference");
        text.remove_prefix(first);
        const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),*value);
        if(error!=std::errc{} || end==text.data()) return invalid("Invalid camera preference");
        text.remove_prefix(static_cast<std::size_t>(end-text.data()));
    }
    if(version7) {
        if(text.empty() || (text.front()!=' ' && text.front()!='\r' && text.front()!='\n')) return invalid("Missing UI scale separator");
        const auto first=text.find_first_not_of(" \r\n");
        if(first==std::string_view::npos) return invalid("Missing UI scale");
        text.remove_prefix(first);
        const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),result.ui_scale_percent);
        if(error!=std::errc{} || end==text.data()) return invalid("Invalid UI scale");
        text.remove_prefix(static_cast<std::size_t>(end-text.data()));
    }
    if (version8) {
        const auto read = [&](auto& value) {
            if (text.empty() || (text.front() != ' ' && text.front() != '\r' && text.front() != '\n')) return false;
            const auto first = text.find_first_not_of(" \r\n");
            if (first == std::string_view::npos) return false;
            text.remove_prefix(first);
            const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), value);
            if (error != std::errc{} || end == text.data()) return false;
            text.remove_prefix(static_cast<std::size_t>(end-text.data()));
            return true;
        };
        unsigned moves{};
        if (!read(result.camera_drag.pan) || !read(result.camera_drag.forward) || !read(result.camera_drag.rotation) || !read(moves) || moves > 1)
            return invalid("Invalid camera navigation preferences");
        result.scroll_moves_camera = moves != 0;
    }
    if (text.find_first_not_of(" \r\n") != std::string_view::npos)
        return invalid("Unexpected data after editor settings");
    if (auto valid = validate_settings(result); !valid)
        return std::unexpected(valid.error());
    return result;
}
std::filesystem::path settings_path() {
    if (const auto* root = std::getenv("XDG_CONFIG_HOME"); root && *root && std::filesystem::path(root).is_absolute())
        return std::filesystem::path(root) / "vng/editor.settings";
    if (const auto* root = std::getenv("HOME"); root && *root)
        return std::filesystem::path(root) / ".config/vng/editor.settings";
    return "editor.settings";
}
vng::content::Result<Settings> load_settings(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) && !error)
        return Settings{};
    if (error || !std::filesystem::is_regular_file(path, error) || error)
        return invalid("Cannot read editor settings: " + path.string());
    if (std::filesystem::file_size(path, error) > 256 || error)
        return invalid("Editor settings file is too large or unreadable");
    std::ifstream file(path, std::ios::binary);
    std::array<char, 257> bytes{};
    file.read(bytes.data(), bytes.size());
    if (file.bad() || !file.eof())
        return invalid("Cannot read editor settings: " + path.string());
    return decode_settings({bytes.data(), static_cast<std::size_t>(file.gcount())});
}
vng::content::Result<void> save_settings(const std::filesystem::path& path, const Settings& settings) {
    if (auto valid = validate_settings(settings); !valid) return valid;
    std::error_code error;
    const auto parent = path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path();
    std::filesystem::create_directories(parent, error);
    if (error) return invalid("Cannot create editor settings directory: " + error.message());
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return invalid("Cannot inspect editor settings destination: " + error.message());
    if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))
        return invalid("Editor settings destination must be a regular file");
    std::string filename = (parent / ".editor-settings-XXXXXX").string();
    const int fd = ::mkstemp(filename.data());
    if (fd < 0) return invalid("Cannot create temporary editor settings file");
    struct Temporary {
        int fd; std::string path;
        ~Temporary() { if (fd >= 0) ::close(fd); ::unlink(path.c_str()); }
    } temporary{fd, filename};
    const auto bytes = encode_settings(settings);
    std::size_t offset{};
    while (offset != bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return invalid("Cannot write editor settings");
        offset += static_cast<std::size_t>(count);
    }
    if (::fsync(fd) != 0) return invalid("Cannot flush editor settings");
    const auto closed = ::close(fd);
    temporary.fd = -1;
    if (closed != 0 || ::rename(filename.c_str(), path.c_str()) != 0)
        return invalid("Cannot publish editor settings");
    return {};
}
} // namespace editor_example
