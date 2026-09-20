#pragma once

#include <vng/core/types.hpp>

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>
#include <utility>

namespace vng::timeline {

using Value = std::variant<bool, i32, u32, f32, Vec3, std::string>;

inline constexpr f32 max_time = 86400.0F;
inline constexpr std::size_t max_keys_per_track = 4096;
inline constexpr std::size_t max_total_keys = 16384;
// Every track has at least one key. Application-specific authoring budgets
// belong to the editor/session, not to portable scene data or playback.
inline constexpr std::size_t max_tracks = max_total_keys;
inline constexpr std::size_t max_property_bytes = 256;
inline constexpr std::size_t max_label_bytes = 256;
inline constexpr std::size_t max_layer_bytes = 128;
inline constexpr std::size_t max_string_value_bytes = 4096;

enum class Interpolation { hold, linear };

struct Target final {
    u64 object{};
    std::string property;
    friend bool operator==(const Target&, const Target&) = default;
};

struct Keyframe final {
    f32 time{};
    Value value;
    // The arriving key owns the interpolation of the preceding segment.
    Interpolation incoming{Interpolation::hold};
    friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

struct Track final {
    Target target;
    std::string label;
    // Organizational metadata only: layers do not disable or alter sampling.
    std::string layer;
    std::vector<Keyframe> keys;
    friend bool operator==(const Track&, const Track&) = default;
};

enum class ErrorCode {
    invalid_target,
    invalid_metadata,
    invalid_time,
    invalid_value,
    invalid_interpolation,
    type_mismatch,
    duplicate_target,
    duplicate_time,
    empty_track,
    not_found,
    limit_exceeded,
};

struct Diagnostic final {
    ErrorCode code{};
    std::string message;
    std::optional<Target> target;
    std::optional<f32> time;
    friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
};

template<class T>
using Result = std::expected<T, Diagnostic>;

// An owning, backend-independent collection of property tracks. Objects and
// properties are application identities; evaluation returns values, never
// invokes effects or mutates the addressed objects. References/spans returned
// below may be invalidated by any mutation.
class Timeline final {
public:
    Timeline() = default;
    Timeline(const Timeline&) = default;
    Timeline& operator=(const Timeline&) = default;
    Timeline(Timeline&& other) noexcept
        : tracks_(std::move(other.tracks_)), version_(std::exchange(other.version_, 0)) {
        other.tracks_.clear();
    }
    Timeline& operator=(Timeline&& other) noexcept {
        if (this != &other) {
            tracks_ = std::move(other.tracks_);
            version_ = std::exchange(other.version_, 0);
            other.tracks_.clear();
        }
        return *this;
    }
    // Process-local content token for derived caches, not a saved document revision.
    // Copies share a token until edited; independently edited copies cannot alias.
    [[nodiscard]] u64 version() const noexcept { return version_; }
    [[nodiscard]] std::span<const Track> tracks() const noexcept { return tracks_; }
    [[nodiscard]] const Track* find(const Target&) const noexcept;

    // Inserts a key or replaces the key at exactly the same time. Empty label
    // and layer arguments preserve existing metadata; replace() can clear it.
    // A track retains its value type, even when replacing its only key.
    [[nodiscard]] Result<void> set(Target, Keyframe, std::string label = {},
                                   std::string layer = {});
    // Removing the last key also removes its track. Invalid/missing keys return false.
    [[nodiscard]] bool erase(const Target&, f32 time);
    // Removes one complete property track without touching other tracks.
    [[nodiscard]] bool erase(const Target&);
    // Moving to an occupied time fails. Moving an existing key to itself is a no-op.
    [[nodiscard]] Result<void> move(const Target&, f32 from, f32 to);
    // Validates everything before committing; tracks and keys are sorted by
    // (object, property) and time. Duplicate targets/times and empty tracks fail.
    [[nodiscard]] Result<void> replace(std::vector<Track>);
    // Validates/replaces only one complete track (or inserts it if absent).
    // Unrelated key vectors retain their allocations and are never copied.
    [[nodiscard]] Result<void> replace_track(Track);

    // Before the first key (or for invalid query times) there is no override.
    // Exact keys return their own value; after the final key its value is held.
    // Linear interpolation is supported only for f32 and Vec3. Other types step.
    [[nodiscard]] std::optional<Value> sample(const Target&, f32 time) const;
    friend bool operator==(const Timeline& a, const Timeline& b) { return a.tracks_ == b.tracks_; }

private:
    std::vector<Track> tracks_;
    u64 version_{};
    void changed() noexcept;
};

} // namespace vng::timeline
