#include "camera_edits.hpp"
#include "project.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace editor_example {
namespace {
using namespace vng;
constexpr std::array<char, 8> magic{'V', 'N', 'G', 'C', 'A', 'M', 0, 3};
constexpr std::size_t payload_size = 84;
constexpr u64 max_revision = (u64{1} << 53) - 1;
static_assert(sizeof(f32) == sizeof(u32) && std::numeric_limits<f32>::is_iec559);

auto invalid(std::string message) {
    content::Diagnostic diagnostic;
    diagnostic.code = content::ErrorCode::invalid_document;
    diagnostic.message = std::move(message);
    return std::unexpected(std::move(diagnostic));
}
content::Result<void> validate(const CameraEdit& edit) {
    if (!edit.base_revision || edit.revision <= edit.base_revision || edit.revision > max_revision)
        return invalid("Camera edit requires 1 <= base < target <= 2^53-1");
    if (!valid_camera_pose(edit.editor_camera) || !valid_camera_pose(edit.animation_camera))
        return invalid("Camera edit poses must be finite and within editor navigation limits");
    return {};
}
void integer(std::string& bytes, u64 value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<char>((value >> (i * 8U)) & 255U));
}
u64 integer(std::string_view bytes, std::size_t& offset, unsigned width) {
    // decode_camera_edit checks the exact fixed length before any reads.
    u64 value{};
    for (unsigned i = 0; i < width; ++i)
        value |= static_cast<u64>(static_cast<unsigned char>(bytes[offset++])) << (i * 8U);
    return value;
}
} // namespace

vng::content::Result<std::string> encode_camera_edit(const CameraEdit& edit) {
    if (auto valid = validate(edit); !valid) return std::unexpected(valid.error());
    std::string bytes;
    bytes.reserve(payload_size);
    bytes.append(magic.data(), magic.size());
    integer(bytes, edit.base_revision, 8);
    integer(bytes, edit.revision, 8);
    for (const auto& pose : {edit.editor_camera, edit.animation_camera})
        for (const auto value : {pose.yaw, pose.pitch, pose.distance,
                                pose.target.x, pose.target.y, pose.target.z, pose.zoom})
            integer(bytes, std::bit_cast<vng::u32>(value), 4);
    integer(bytes, edit.pilot_camera ? 1 : 0, 4);
    return bytes;
}

vng::content::Result<CameraEdit> decode_camera_edit(std::string_view bytes) {
    const bool legacy = bytes.size() == 76 && bytes.substr(0,8) == std::string_view{"VNGCAM\0\2",8};
    if (!legacy && (bytes.size() != payload_size ||
        bytes.substr(0, magic.size()) != std::string_view(magic.data(), magic.size())))
        return invalid("Camera edit payload has an unknown magic or version");
    std::size_t offset = magic.size();
    CameraEdit edit;
    edit.base_revision = integer(bytes, offset, 8);
    edit.revision = integer(bytes, offset, 8);
    for (auto* pose : {&edit.editor_camera, &edit.animation_camera}) {
        pose->yaw = std::bit_cast<vng::f32>(static_cast<vng::u32>(integer(bytes, offset, 4)));
        pose->pitch = std::bit_cast<vng::f32>(static_cast<vng::u32>(integer(bytes, offset, 4)));
        pose->distance = std::bit_cast<vng::f32>(static_cast<vng::u32>(integer(bytes, offset, 4)));
        pose->target.x = std::bit_cast<vng::f32>(static_cast<vng::u32>(integer(bytes, offset, 4)));
        pose->target.y = std::bit_cast<vng::f32>(static_cast<vng::u32>(integer(bytes, offset, 4)));
        pose->target.z = std::bit_cast<vng::f32>(static_cast<vng::u32>(integer(bytes, offset, 4)));
        if (!legacy) pose->zoom = std::bit_cast<vng::f32>(static_cast<vng::u32>(integer(bytes, offset, 4)));
    }
    const auto pilot = integer(bytes, offset, 4);
    if (pilot > 1) return invalid("Camera edit pilot flag must be 0 or 1");
    edit.pilot_camera = pilot == 1;
    if (auto valid = validate(edit); !valid) return std::unexpected(valid.error());
    return edit;
}

vng::content::Result<CameraEdit> camera_edit(vng::u64 base_revision, const State& current) {
    CameraEdit edit{base_revision, current.document.revision, current.viewport.editor_camera,
                    current.document.animation_camera, current.viewport.pilot_camera};
    if (auto valid = validate(edit); !valid) return std::unexpected(valid.error());
    return edit;
}

vng::content::Result<void> apply_camera_edit(State& state, const CameraEdit& edit) {
    if (auto valid = validate(edit); !valid) return valid;
    if (state.document.revision != edit.base_revision)
        return invalid("Stale camera edit base revision; resynchronize before applying");
    state.viewport.editor_camera = edit.editor_camera;
    state.document.animation_camera = edit.animation_camera;
    state.viewport.pilot_camera = edit.pilot_camera;
    state.document.revision = edit.revision;
    return {};
}
} // namespace editor_example
