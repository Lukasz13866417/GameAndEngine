#pragma once
#include <vng/content/document.hpp>
#include <vng/core/types.hpp>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// A review of one or more saved scenes: what the owner thinks of each
// candidate, and notes pinned to things they clicked at a moment in a shot.
// Stored as a "vreview 1.0" document next to (or near) the scenes it names;
// an agent can read and answer it, and the vng_review app writes it back.
namespace review {
// One scene under review. Its clock is the review clock plus `offset`.
struct Candidate {
    std::string id;       // short and unique: "A", "B", ...
    std::string label;    // what it is, in a few words
    std::string about;    // the author's description of the variant
    std::string thoughts; // the reviewer's thoughts on it
    std::filesystem::path scene;
    vng::f32 offset{};
    friend bool operator==(const Candidate&, const Candidate&) = default;
};
// A note pinned to what the reviewer clicked in a candidate's view. On a
// character (a skinned .vmesh rather than a scene) the note also keeps the
// clip and the camera, the object is 1 for the character, its name is the
// bone that moves the skin there most, and the point is in the bind pose.
struct Note {
    vng::u32 id{};
    std::string candidate;
    vng::f32 time{};                  // on the review clock
    vng::u32 object{};                // scene instance (or 1 for a character), 0 for the background
    std::string object_name;
    std::optional<vng::Vec3> point;   // where the click met the object: scene units, or a character's bind pose
    vng::Vec2 view{};                 // where the click was, normalized top-left
    std::string text;
    std::string created;              // UTC, ISO 8601
    std::string status{"open"};       // "open" or "resolved"
    std::string reply;                // the author's answer
    std::string clip;                 // a character's clip, "" for a scene
    // A character's camera: yaw and pitch (degrees), distance (m), and the point looked at (m).
    std::optional<std::array<vng::f32, 6>> camera;
    friend bool operator==(const Note&, const Note&) = default;
};
struct Review {
    std::string title;
    std::string created;
    std::optional<std::array<vng::f32, 2>> range; // review clock; whole scenes when absent
    std::string summary;                          // the reviewer's overall thoughts
    std::string pick;                             // chosen candidate id, comparisons only
    std::string verdict;                          // "", "approved" or "needs work"
    std::vector<Candidate> candidates;
    std::vector<Note> notes;
    friend bool operator==(const Review&, const Review&) = default;
    [[nodiscard]] bool comparison() const { return candidates.size() > 1; }
    [[nodiscard]] const Candidate* find(std::string_view id) const;
    [[nodiscard]] vng::u32 next_note_id() const;
};
inline constexpr std::size_t max_candidates = 4, max_notes = 4096, max_text_bytes = 16 * 1024;

// One version of a file: any write, in place or by rename, gives a new stamp.
struct FileStamp {
    vng::u64 device{}, inode{}, size{};
    vng::i64 modified_ns{};
    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};
// Nothing when the file does not exist or cannot be inspected.
[[nodiscard]] std::optional<FileStamp> stamp_of(const std::filesystem::path&);

// Scene paths are resolved against the review file's directory on read and
// written relative to it where possible.
[[nodiscard]] vng::content::Result<Review> parse_review(std::string_view source, const std::filesystem::path& file);
[[nodiscard]] vng::content::Result<Review> read_review(const std::filesystem::path& file);
[[nodiscard]] vng::content::Result<std::string> encode_review(const Review&, const std::filesystem::path& file);
// Replaces the file atomically (temporary file in the same directory, then
// rename), keeping its permissions, and returns the stamp of what it wrote.
// A symbolic link stays a link: the file it names is replaced.
// If backup is supplied, first preserve the existing bytes there. Never
// overwrite a backup; if preservation fails, leave the review untouched.
[[nodiscard]] vng::content::Result<FileStamp> save_review(const std::filesystem::path& file, const Review&,
    const std::optional<std::filesystem::path>& backup = {});
[[nodiscard]] vng::content::Result<void> validate(const Review&);
// Folds the edits someone else made to the file (theirs: an agent's replies,
// say) into the edits made here (mine) since both last matched (base). Each
// field keeps the side that changed it, and mine when both did. Candidates
// and notes match by id: one added on either side is kept, and one deleted
// on either side stays deleted unless the other side changed it. A note both
// sides added under the same id keeps it on theirs; mine gets the next free
// id. Notes on a candidate that is gone are dropped, as is a pick of one.
[[nodiscard]] Review merge(const Review& base, const Review& mine, const Review& theirs);
// The current UTC time as "2026-09-28T18:40:00Z".
[[nodiscard]] std::string timestamp();
} // namespace review
