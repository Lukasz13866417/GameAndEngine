#include "spaceflight_scene.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>

namespace example::spaceflight {
namespace {

using namespace vng;
using content::Reader;

[[noreturn]] void fail_at(const Reader& reader, std::string_view key,
    std::string message)
{
    if (reader.view().child(key)) reader.child(key).fail(std::move(message));
    reader.fail(std::move(message));
}

void range(const Reader& reader, std::string_view key, f32 value, f32 low, f32 high)
{
    if (!std::isfinite(value) || value < low || value > high) {
        fail_at(reader, key, std::string(key) + " must be finite and in ["
            + std::to_string(low) + ", " + std::to_string(high) + "]");
    }
}

void position(const Reader& reader, std::string_view key, Vec3 value)
{
    for (std::size_t i = 0; i < 3; ++i) range(reader, key, value[i], -10000, 10000);
}

content::Diagnostic option_error(std::string message)
{
    return {
        .code = content::ErrorCode::invalid_document,
        .message = std::move(message),
        .location = {},
        .path = {},
        .notes = {std::string(usage())},
    };
}

template<class T>
bool number(std::string_view text, T& result)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 scale(Vec3 a, f32 s) { return {a.x*s, a.y*s, a.z*s}; }
f32 dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b)
{ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
Vec3 unit(Vec3 a) { return scale(a, 1.0F/std::sqrt(dot(a,a))); }

} // namespace

// ADL decoding stays next to this example's types. The document library has no
// camera, flight, or bloom concepts and leaves all other attributes untouched.
content::Result<CameraSettings> decode(content::NodeView node,
    content::Type<CameraSettings>)
{
    return node.read([](Reader& r) {
        CameraSettings camera;
        camera.position = r.get_or<Vec3>("position", camera.position);
        camera.target = r.get_or<Vec3>("target", camera.target);
        camera.vertical_fov_degrees = r.get_or<f32>("vertical_fov_degrees", 38);
        camera.near_plane = r.get_or<f32>("near_plane", 0.1F);
        camera.far_plane = r.get_or<f32>("far_plane", 1000);
        position(r, "position", camera.position);
        position(r, "target", camera.target);
        range(r, "vertical_fov_degrees", camera.vertical_fov_degrees, 5, 150);
        range(r, "near_plane", camera.near_plane, 0.001F, 1000);
        range(r, "far_plane", camera.far_plane, 0.002F, 100000);
        if (camera.far_plane <= camera.near_plane) {
            fail_at(r, "far_plane", "far_plane must be greater than near_plane");
        }
        const auto dx = camera.target.x - camera.position.x;
        const auto dz = camera.target.z - camera.position.z;
        if (dx * dx + dz * dz < 1.0e-8F) {
            fail_at(r, "target", "Camera direction must not be parallel to its +Y up axis");
        }
        return camera;
    });
}

content::Result<FlightSettings> decode(content::NodeView node,
    content::Type<FlightSettings>)
{
    return node.read([](Reader& r) {
        FlightSettings flight;
        flight.start = r.get_or<Vec3>("start", flight.start);
        flight.end = r.get_or<Vec3>("end", flight.end);
        flight.duration = r.get_or<f32>("duration", flight.duration);
        flight.loop = r.get_or<bool>("loop", flight.loop);
        flight.bank_degrees = r.get_or<f32>("bank_degrees", flight.bank_degrees);
        if (r.view().child("controls")) {
            auto controls = r.child("controls");
            std::array<Vec3, 2> points;
            std::size_t index{};
            for (auto element : controls.elements()) {
                if (index == points.size()) controls.fail("controls needs exactly two Bezier handles");
                points[index++] = element.as<Vec3>();
            }
            if (index != points.size()) controls.fail("controls needs exactly two Bezier handles");
            for (const auto p : points) position(r, "controls", p);
            flight.controls = points;
        }
        position(r, "start", flight.start);
        position(r, "end", flight.end);
        range(r, "duration", flight.duration, 0.1F, 3600);
        range(r, "bank_degrees", flight.bank_degrees, 0, 30);
        if (flight.start == flight.end) fail_at(r, "end", "The flyby needs distinct start and end positions");
        if (flight.controls) {
            // Ordered progress along the chord guarantees a nonzero tangent
            // everywhere, including the endpoints, without a frame-history fallback.
            const auto chord = sub(flight.end, flight.start);
            const auto& p = *flight.controls;
            for (auto edge : {sub(p[0], flight.start), sub(p[1], p[0]), sub(flight.end, p[1])})
                if (dot(edge, chord) <= 1.0e-5F)
                    fail_at(r, "controls", "Bezier handles must progress from start toward end");
        }
        return flight;
    });
}

content::Result<SunSettings> decode(content::NodeView node, content::Type<SunSettings>)
{
    return node.read([](Reader& r) {
        SunSettings sun;
        sun.position = r.get_or<Vec3>("position", sun.position);
        sun.radius = r.get_or<f32>("radius", sun.radius);
        sun.white_spots = r.get_or<bool>("white_spots", false);
        position(r, "position", sun.position);
        range(r, "radius", sun.radius, 0.001F, 10000);
        return sun;
    });
}

content::Result<Scene> decode_scene(const content::Document& document)
{
    if (document.kind() != "vscene" || document.version() != content::DocumentVersion{1, 0}) {
        return std::unexpected(document.root().error(content::ErrorCode::unsupported_version,
            "The spaceflight example expects a vscene 1.0 document"));
    }
    return document.read([&](Reader& r) {
        Scene scene;
        auto mesh = r.get<std::string>("mesh");
        if (mesh.empty() || mesh.find('\0') != std::string::npos) {
            r.child("mesh").fail("mesh must be a nonempty file path without NUL characters");
        }
        scene.mesh_path = std::filesystem::path(std::move(mesh));
        if (scene.mesh_path.is_relative()) {
            scene.mesh_path = document.source_path().parent_path() / scene.mesh_path;
        }
        scene.mesh_path = scene.mesh_path.lexically_normal();
        scene.camera = r.get_or<CameraSettings>("camera", {});
        scene.flight = r.get_or<FlightSettings>("flight", {});
        if (r.view().child("sun")) scene.sun = r.get<SunSettings>("sun");
        scene.hero_time = r.get_or<f32>("hero_time", std::min(10.7F, scene.flight.duration));
        range(r, "hero_time", scene.hero_time, 0, scene.flight.duration);
        const auto size = r.get_or<UVec2>("extent", {1280, 800});
        if (size.x < 64 || size.y < 64 || size.x > 8192 || size.y > 8192) {
            fail_at(r, "extent", "Both viewport dimensions must be in [64, 8192]");
        }
        scene.extent = {size.x, size.y};

        // Optional sections still reject an explicitly malformed value.
        for (auto member : r.members()) {
            if (member.name == "background") {
                auto b = member.value;
                scene.star_count = b.get_or<u32>("stars", 700);
                scene.star_seed = b.get_or<u32>("seed", 32);
                if (scene.star_count > 20000) fail_at(b, "stars", "stars must not exceed 20000");
            } else if (member.name == "bloom") {
                auto b = member.value;
                scene.bloom.threshold = b.get_or<f32>("threshold", 1);
                scene.bloom.strength = b.get_or<f32>("strength", 0.22F);
                scene.bloom.exposure = b.get_or<f32>("exposure", 1.1F);
                range(b, "threshold", scene.bloom.threshold, 0, 100);
                range(b, "strength", scene.bloom.strength, 0, 5);
                range(b, "exposure", scene.bloom.exposure, 0.01F, 20);
            }
        }
        return scene;
    });
}

content::Result<Scene> load_scene(const std::filesystem::path& path)
{
    auto document = content::read_document(path);
    if (!document) return std::unexpected(std::move(document.error()));
    return decode_scene(*document);
}

Mat4 Scene::transform_at(f32 seconds) const noexcept
{
    if (!std::isfinite(seconds)) seconds = 0;
    seconds = std::max(seconds, 0.0F);
    const auto duration = std::max(flight.duration, 0.1F);
    const auto time = flight.loop ? std::fmod(seconds, duration) : std::min(seconds, duration);
    const auto t = time / duration;
    if (flight.controls) {
        const auto& p = *flight.controls;
        const auto u = 1.0F - t;
        const auto location = add(add(scale(flight.start, u*u*u), scale(p[0], 3*u*u*t)),
            add(scale(p[1], 3*u*t*t), scale(flight.end, t*t*t)));
        const auto tangent = add(add(scale(sub(p[0], flight.start), 3*u*u),
            scale(sub(p[1], p[0]), 6*u*t)), scale(sub(flight.end, p[1]), 3*t*t));
        // Local -Z is the nose. Bank about the actual flight tangent, not a
        // world axis; sampling any time is independent of previous frames.
        const auto forward = unit(tangent);
        const Vec3 hint = std::abs(forward.y) > 0.98F ? Vec3{0,0,1} : Vec3{0,1,0};
        const auto right = unit(cross(forward, hint));
        const auto up = cross(right, forward);
        const auto bank = -std::sin(t * std::numbers::pi_v<f32>)
            * flight.bank_degrees * std::numbers::pi_v<f32> / 180.0F;
        const auto x = add(scale(right, std::cos(bank)), scale(up, std::sin(bank)));
        const auto y = add(scale(right, -std::sin(bank)), scale(up, std::cos(bank)));
        return Mat4{{Vec4{x.x,x.y,x.z,0}, Vec4{y.x,y.y,y.z,0},
            Vec4{-forward.x,-forward.y,-forward.z,0}, Vec4{location.x,location.y,location.z,1}}};
    }
    Vec3 location;
    for (std::size_t i = 0; i < 3; ++i) {
        location[i] = flight.start[i] + (flight.end[i] - flight.start[i]) * t;
    }

    const auto phase = t * 2 * std::numbers::pi_v<f32>;
    const auto amplitude = flight.bank_degrees * std::numbers::pi_v<f32> / 180;
    const auto yaw = std::sin(phase) * amplitude * 0.55F;
    const auto pitch = std::sin(phase * 0.5F) * amplitude * 0.3F;
    const auto roll = std::sin(phase + 0.4F) * amplitude;
    const auto cy = std::cos(yaw), sy = std::sin(yaw);
    const auto cx = std::cos(pitch), sx = std::sin(pitch);
    const auto cz = std::cos(roll), sz = std::sin(roll);

    // Column-major R_yaw * R_pitch * R_roll, followed by translation.
    Mat4 transform;
    transform[0] = {cy * cz + sy * sx * sz, cx * sz, -sy * cz + cy * sx * sz, 0};
    transform[1] = {-cy * sz + sy * sx * cz, cx * cz, sy * sz + cy * sx * cz, 0};
    transform[2] = {sy * cx, -sx, cy * cx, 0};
    transform[3] = {location.x, location.y, location.z, 1};
    return transform;
}

std::string_view usage() noexcept
{
    return "vng_spaceflight_demo / vng_solar_flyby_demo [scene.vscene] [--once | --frames N] [--time SECONDS] "
        "[--analyze NEW_DIRECTORY] [--screenshot IMAGE.png] [--no-bloom] [--help]\n";
}

content::Result<Options> parse_options(int argc, char** argv,
    std::filesystem::path default_scene)
{
    Options options{.scene_path = std::move(default_scene),
        .frame_limit = {}, .fixed_time = {}, .analysis_directory = {}, .screenshot_path = {}};
    if (argc < 1 || !argv) return std::unexpected(option_error("Invalid argument list"));
    bool has_scene = false;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) return std::unexpected(option_error("Invalid null argument"));
        const std::string_view argument{argv[i]};
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--once") {
            if (options.frame_limit) return std::unexpected(option_error("Specify only one frame limit"));
            options.frame_limit = 1;
        } else if (argument == "--no-bloom") {
            options.bloom = false;
        } else if (argument == "--frames" || argument == "--time"
            || argument == "--analyze" || argument == "--screenshot") {
            if (i + 1 >= argc || !argv[i + 1]) {
                return std::unexpected(option_error(std::string(argument) + " requires a value"));
            }
            const std::string_view value{argv[++i]};
            if (argument == "--frames") {
                u64 count{};
                if (options.frame_limit || !number(value, count) || count == 0) {
                    return std::unexpected(option_error("--frames requires one positive integer"));
                }
                options.frame_limit = count;
            } else if (argument == "--time") {
                f32 seconds{};
                if (options.fixed_time || !number(value, seconds)
                    || !std::isfinite(seconds) || seconds < 0 || seconds > 86400) {
                    return std::unexpected(option_error("--time requires one finite value in [0, 86400] seconds"));
                }
                options.fixed_time = seconds;
            } else {
                auto& destination = argument == "--analyze"
                    ? options.analysis_directory : options.screenshot_path;
                if (destination || value.empty() || value.starts_with('-')) {
                    return std::unexpected(option_error(std::string(argument) + " requires one nonempty output path"));
                }
                destination = std::filesystem::path(value);
            }
        } else if (!argument.empty() && !argument.starts_with('-') && !has_scene) {
            options.scene_path = std::filesystem::path(argument);
            has_scene = true;
        } else {
            return std::unexpected(option_error("Unrecognized or duplicate argument: " + std::string(argument)));
        }
    }
    return options;
}

} // namespace example::spaceflight
