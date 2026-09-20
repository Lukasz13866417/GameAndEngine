#pragma once

#include "project.hpp"
#include <string>
#include <string_view>
#include <vng/content/diagnostic.hpp>
#include <vng/core/types.hpp>

namespace editor_example {
// Complete absolute camera state, independent of mesh size. Generation
// identity comes from the enclosing preview connection, just like vertices.
struct CameraEdit {
    vng::u64 base_revision{}, revision{};
    CameraPose editor_camera{}, animation_camera{};
    bool pilot_camera{};
    friend bool operator==(const CameraEdit&, const CameraEdit&) = default;
};

// Fixed 84-byte v3 payload: 8-byte magic/version, two LE u64 revisions,
// two sets of seven LE IEEE754 binary32 values (yaw, pitch, distance, target x/y/z, zoom),
// followed by a LE u32 pilot flag, strictly 0 or 1.
// The containing control message adds the "camera\n" prefix.
// Legacy v2 payloads decode with optical zoom = 1.
[[nodiscard]] vng::content::Result<std::string> encode_camera_edit(const CameraEdit&);
[[nodiscard]] vng::content::Result<CameraEdit> decode_camera_edit(std::string_view);

// Caller declares that only the camera changed since this acknowledged base.
// Reads two small poses and a revision; never scans or copies the mesh/document.
[[nodiscard]] vng::content::Result<CameraEdit> camera_edit(vng::u64 base_revision, const State& current);

// Validates all fields and exact base revision before writing any field.
// Scene/mesh data, selection, playback, and unrelated settings stay untouched.
[[nodiscard]] vng::content::Result<void> apply_camera_edit(State&, const CameraEdit&);
} // namespace editor_example
