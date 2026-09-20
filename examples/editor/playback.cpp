#include "playback.hpp"
#include "project.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace editor_example {
namespace {
using namespace vng;
constexpr std::array<char, 8> magic{'V', 'N', 'G', 'T', 'I', 'M', 0, 1};
constexpr std::size_t payload_size = 29;
auto invalid(std::string message) {
    content::Diagnostic diagnostic;
    diagnostic.code = content::ErrorCode::invalid_document;
    diagnostic.message = std::move(message);
    return std::unexpected(std::move(diagnostic));
}
content::Result<void> validate(const PlaybackEdit& edit) {
    if (!edit.base_revision || edit.revision <= edit.base_revision ||
        edit.revision > (u64{1} << 53) - 1)
        return invalid("Playback edit requires 1 <= base < target <= 2^53-1");
    if (!std::isfinite(edit.time) || edit.time < 0 || edit.time > 86400)
        return invalid("Playback time must be finite and between 0 and 86400 seconds");
    return {};
}
void integer(std::string& bytes, u64 value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<char>((value >> (i * 8U)) & 255U));
}
u64 integer(std::string_view bytes, std::size_t& offset, unsigned width) {
    u64 value{};
    for (unsigned i = 0; i < width; ++i)
        value |= static_cast<u64>(static_cast<unsigned char>(bytes[offset++])) << (i * 8U);
    return value;
}
} // namespace
vng::content::Result<PlaybackEdit> playback_edit(vng::u64 base, const State& state) {
    PlaybackEdit edit{base, state.document.revision, state.viewport.time, state.viewport.paused};
    if (auto valid = validate(edit); !valid)
        return std::unexpected(valid.error());
    return edit;
}
vng::content::Result<std::string> encode_playback_edit(const PlaybackEdit& edit) {
    if (auto valid = validate(edit); !valid)
        return std::unexpected(valid.error());
    std::string bytes(magic.data(), magic.size());
    bytes.reserve(payload_size);
    integer(bytes, edit.base_revision, 8);
    integer(bytes, edit.revision, 8);
    integer(bytes, std::bit_cast<vng::u32>(edit.time), 4);
    bytes.push_back(edit.paused ? 1 : 0);
    return bytes;
}
vng::content::Result<PlaybackEdit> decode_playback_edit(std::string_view bytes) {
    if (bytes.size() != payload_size ||
        bytes.substr(0, magic.size()) != std::string_view(magic.data(), magic.size()))
        return invalid("Playback edit requires a versioned 29-byte payload");
    if (bytes.back() != 0 && bytes.back() != 1)
        return invalid("Playback paused flag must be zero or one");
    std::size_t offset = magic.size();
    PlaybackEdit edit;
    edit.base_revision = integer(bytes, offset, 8);
    edit.revision = integer(bytes, offset, 8);
    edit.time = std::bit_cast<vng::f32>(static_cast<vng::u32>(integer(bytes, offset, 4)));
    edit.paused = bytes.back() != 0;
    if (auto valid = validate(edit); !valid)
        return std::unexpected(valid.error());
    return edit;
}
vng::content::Result<void> apply_playback_edit(State& state, const PlaybackEdit& edit) {
    if (auto valid = validate(edit); !valid)
        return valid;
    if (state.document.revision != edit.base_revision)
        return invalid("Stale playback edit base revision; resynchronize before applying");
    if (edit.time > state.document.timeline_duration)
        return invalid("Playback time exceeds the scene timeline duration");
    state.viewport.time = edit.time;
    state.viewport.paused = edit.paused;
    state.document.revision = edit.revision;
    return {};
}
double advance_playback(double time, double delta, vng::f32 duration) {
    if (!std::isfinite(time) || !std::isfinite(delta) || !std::isfinite(duration) ||
        duration <= 0 || delta < 0)
        return time;
    return std::fmod(std::max(0.0, time) + std::fmod(delta, duration), duration);
}
} // namespace editor_example
