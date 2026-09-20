#pragma once
#include "scale_limits.hpp"
#include <vng/content/diagnostic.hpp>
#include <vng/timeline/timeline.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace editor_example {
struct State;
// Uniform instance scale, never a mutation of blueprint vertices.
struct ScaleSnapshot {
    vng::u32 object{};
    vng::f32 base_scale{1};
    std::optional<vng::timeline::Track> track;
    friend bool operator==(const ScaleSnapshot&, const ScaleSnapshot&) = default;
};
struct ScaleEdit {
    vng::u64 base_revision{}, revision{};
    ScaleSnapshot scale;
    friend bool operator==(const ScaleEdit&, const ScaleEdit&) = default;
};
[[nodiscard]] vng::content::Result<ScaleSnapshot> capture_scale(const State&, vng::u32);
[[nodiscard]] vng::content::Result<void> restore_scale(State&, const ScaleSnapshot&);
[[nodiscard]] vng::content::Result<bool> apply_scale_value(State&, vng::u32, vng::f32);
[[nodiscard]] vng::content::Result<bool> apply_axis_scale_value(State&, vng::u32, vng::Vec3);
[[nodiscard]] vng::content::Result<ScaleEdit> scale_edit(vng::u64 base, const State&, vng::u32);
[[nodiscard]] vng::content::Result<void> apply_scale_edit(State&, const ScaleEdit&);
// 40-byte LE header; an optional scale track adds metadata and 12 bytes/key.
[[nodiscard]] vng::content::Result<std::string> encode_scale_edit(const ScaleEdit&);
[[nodiscard]] vng::content::Result<ScaleEdit> decode_scale_edit(std::string_view);
}
