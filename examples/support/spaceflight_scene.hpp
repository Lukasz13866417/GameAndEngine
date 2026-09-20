#pragma once

#include <filesystem>
#include <array>
#include <optional>
#include <string_view>

#include <vng/content/document.hpp>
#include <vng/core/types.hpp>
#include <vng/render/bloom.hpp>

namespace example::spaceflight {

// These are this example's conventions, not a schema built into the engine.
struct CameraSettings final {
    vng::Vec3 position{11, 7, -10};
    vng::Vec3 target{0, 0, 0};
    vng::f32 vertical_fov_degrees{38};
    vng::f32 near_plane{0.1F};
    vng::f32 far_plane{1000};
};

struct FlightSettings final {
    vng::Vec3 start{0, 0, 40};
    vng::Vec3 end{0, 0, -20};
    vng::f32 duration{16};
    bool loop{true};
    vng::f32 bank_degrees{4};
    // Optional cubic Bezier handles. Without them the original flyby is unchanged.
    std::optional<std::array<vng::Vec3, 2>> controls{};
};

struct SunSettings final {
    vng::Vec3 position{-42, 5, -140};
    vng::f32 radius{31};
    bool white_spots{false};
};

struct Scene final {
    std::filesystem::path mesh_path;
    CameraSettings camera{};
    FlightSettings flight{};
    vng::u32 star_count{700};
    vng::u32 star_seed{32};
    vng::render::BloomSettings bloom{1.0F, 0.22F, 1.1F};
    vng::f32 hero_time{10.7F};
    vng::Extent2D extent{1280, 800};
    std::optional<SunSettings> sun{};

    // Seconds since playback began; fixed sample times use exactly this path.
    // The ship points along local -Z with +Y up. No scale is introduced.
    [[nodiscard]] vng::Mat4 transform_at(vng::f32 seconds) const noexcept;
};

struct Options final {
    std::filesystem::path scene_path;
    std::optional<vng::u64> frame_limit;
    std::optional<vng::f32> fixed_time;
    std::optional<std::filesystem::path> analysis_directory;
    std::optional<std::filesystem::path> screenshot_path;
    bool bloom{true};
    bool help{};
};

[[nodiscard]] vng::content::Result<Scene> decode_scene(
    const vng::content::Document& document);
[[nodiscard]] vng::content::Result<Scene> load_scene(
    const std::filesystem::path& path);
[[nodiscard]] vng::content::Result<Options> parse_options(
    int argc, char** argv, std::filesystem::path default_scene);
[[nodiscard]] std::string_view usage() noexcept;

} // namespace example::spaceflight
