#include "review_file.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iomanip>
#include <locale>
#include <random>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace review {
namespace {
using namespace vng;
using content::Reader;

constexpr std::uintmax_t max_file_bytes = 16U << 20;

content::Diagnostic diagnostic(content::ErrorCode code, std::string message, const std::filesystem::path& file = {}) {
    content::Diagnostic error;
    error.code = code;
    error.message = std::move(message);
    error.path = file;
    return error;
}
content::Diagnostic invalid(std::string message) {
    return diagnostic(content::ErrorCode::invalid_document, std::move(message));
}
content::Diagnostic io_failure(const std::filesystem::path& file, std::string_view what, int code = errno) {
    return diagnostic(content::ErrorCode::io_error, std::format("{}: {}", what, std::strerror(code)), file);
}
FileStamp stamp(const struct stat& status) {
    return {static_cast<u64>(status.st_dev), static_cast<u64>(status.st_ino), static_cast<u64>(status.st_size),
            static_cast<i64>(status.st_mtim.tv_sec) * 1'000'000'000 + status.st_mtim.tv_nsec};
}
[[noreturn]] void fail_at(const Reader& reader, std::string_view key, std::string message) {
    if (reader.view().child(key)) reader.child(key).fail(std::move(message));
    reader.fail(std::move(message));
}
// Escapes quotes, backslashes and control characters; newlines stay readable as \n.
std::string quote(std::string_view text) {
    std::string out{"\""};
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            else out += c;
        }
    }
    return out += '"';
}
bool single_line(std::string_view text) {
    return std::ranges::none_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}
bool readable(std::string_view text) {
    return std::ranges::none_of(text, [](char c) {
        return static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t';
    });
}
std::filesystem::path directory_of(const std::filesystem::path& file) {
    return std::filesystem::absolute(file).lexically_normal().parent_path();
}
std::string relative_scene(const std::filesystem::path& scene, const std::filesystem::path& file) {
    const auto relative = std::filesystem::absolute(scene).lexically_normal().lexically_relative(directory_of(file));
    return (relative.empty() ? scene : relative).generic_string();
}

// Three-way merge of one value: mine when I changed it, theirs otherwise.
template <class T> const T& side(const T& base, const T& mine, const T& theirs) {
    return mine != base ? mine : theirs;
}
Candidate merged(const Candidate& b, const Candidate& m, const Candidate& t) {
    return {m.id, side(b.label, m.label, t.label), side(b.about, m.about, t.about),
            side(b.thoughts, m.thoughts, t.thoughts), side(b.scene, m.scene, t.scene), side(b.offset, m.offset, t.offset)};
}
Note merged(const Note& b, const Note& m, const Note& t) {
    return {m.id, side(b.candidate, m.candidate, t.candidate), side(b.time, m.time, t.time),
            side(b.object, m.object, t.object), side(b.object_name, m.object_name, t.object_name),
            side(b.point, m.point, t.point), side(b.view, m.view, t.view), side(b.text, m.text, t.text),
            side(b.created, m.created, t.created), side(b.status, m.status, t.status), side(b.reply, m.reply, t.reply),
            side(b.clip, m.clip, t.clip), side(b.camera, m.camera, t.camera)};
}
// Theirs' order, then what only mine has. `clashes` collects entries both
// sides added under one id, differently; theirs keep the id.
template <class T, class Id>
std::vector<T> merged(const std::vector<T>& base, const std::vector<T>& mine, const std::vector<T>& theirs,
                      Id T::* id, std::vector<T>& clashes) {
    const auto in = [&](const std::vector<T>& list, const Id& key) -> const T* {
        const auto found = std::ranges::find(list, key, id);
        return found == list.end() ? nullptr : &*found;
    };
    std::vector<T> out;
    for (const auto& t : theirs) {
        const auto* b = in(base, t.*id);
        const auto* m = in(mine, t.*id);
        if (m && b) out.push_back(merged(*b, *m, t));
        else if (m) {
            out.push_back(t);
            if (*m != t) clashes.push_back(*m);
        } else if (!b || t != *b) out.push_back(t); // added there, or changed there after I deleted it
    }
    for (const auto& m : mine)
        if (!in(theirs, m.*id))
            if (const auto* b = in(base, m.*id); !b || m != *b) out.push_back(m); // added here, or changed here after they deleted it
    return out;
}
} // namespace

const Candidate* Review::find(std::string_view id) const {
    const auto found = std::ranges::find(candidates, id, &Candidate::id);
    return found == candidates.end() ? nullptr : &*found;
}
u32 Review::next_note_id() const {
    u32 id{};
    for (const auto& note : notes) id = std::max(id, note.id);
    return id + 1;
}

std::string timestamp() {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

content::Result<void> validate(const Review& r) {
    if (r.title.empty() || r.title.size() > 256 || !single_line(r.title))
        return std::unexpected(invalid("The title must be one line of 1 to 256 bytes"));
    if (r.candidates.empty() || r.candidates.size() > max_candidates)
        return std::unexpected(invalid("A review needs 1 to 4 candidates"));
    std::set<std::string_view> ids;
    for (const auto& c : r.candidates) {
        if (c.id.empty() || c.id.size() > 16 || !single_line(c.id) || c.id.find(' ') != std::string::npos)
            return std::unexpected(invalid("Candidate ids must be 1 to 16 bytes without spaces"));
        if (!ids.insert(c.id).second) return std::unexpected(invalid("Duplicate candidate id \"" + c.id + "\""));
        if (c.label.empty() || c.label.size() > 256 || !single_line(c.label))
            return std::unexpected(invalid("Candidate " + c.id + " needs a one-line label of up to 256 bytes"));
        if (c.about.size() > max_text_bytes || c.thoughts.size() > max_text_bytes || !readable(c.about) || !readable(c.thoughts))
            return std::unexpected(invalid("Candidate " + c.id + " has text that is too long or has control characters"));
        if (c.scene.empty()) return std::unexpected(invalid("Candidate " + c.id + " names no scene"));
        if (!std::isfinite(c.offset)) return std::unexpected(invalid("Candidate " + c.id + " has a non-finite offset"));
    }
    if (r.range && (!std::isfinite((*r.range)[0]) || !std::isfinite((*r.range)[1]) || (*r.range)[0] < 0 ||
                    (*r.range)[0] >= (*r.range)[1]))
        return std::unexpected(invalid("The range must be two finite times, from < to, from >= 0"));
    if (r.summary.size() > max_text_bytes || !readable(r.summary))
        return std::unexpected(invalid("The summary is too long or has control characters"));
    if (!r.pick.empty() && !r.find(r.pick)) return std::unexpected(invalid("The pick names no candidate"));
    if (r.verdict != "" && r.verdict != "approved" && r.verdict != "needs work")
        return std::unexpected(invalid("The verdict must be \"\", \"approved\" or \"needs work\""));
    if (r.notes.size() > max_notes) return std::unexpected(invalid("Too many notes"));
    std::set<u32> note_ids;
    for (const auto& n : r.notes) {
        const auto where = "Note " + std::to_string(n.id);
        if (n.id == 0 || !note_ids.insert(n.id).second) return std::unexpected(invalid(where + ": ids must be unique and positive"));
        if (!r.find(n.candidate)) return std::unexpected(invalid(where + " names no candidate"));
        if (!std::isfinite(n.time)) return std::unexpected(invalid(where + " has a non-finite time"));
        if (n.point && (!std::isfinite(n.point->x) || !std::isfinite(n.point->y) || !std::isfinite(n.point->z)))
            return std::unexpected(invalid(where + " has a non-finite point"));
        if (!(n.view.x >= 0 && n.view.x <= 1 && n.view.y >= 0 && n.view.y <= 1))
            return std::unexpected(invalid(where + ": view must be normalized"));
        if (n.text.size() > max_text_bytes || n.reply.size() > max_text_bytes || !readable(n.text) || !readable(n.reply))
            return std::unexpected(invalid(where + " has text that is too long or has control characters"));
        if (n.object_name.size() > 256 || !single_line(n.object_name) || !single_line(n.created))
            return std::unexpected(invalid(where + " has a bad object name or creation time"));
        if (n.status != "open" && n.status != "resolved")
            return std::unexpected(invalid(where + ": status must be \"open\" or \"resolved\""));
        if (n.clip.size() > 256 || !single_line(n.clip)) return std::unexpected(invalid(where + " has a bad clip name"));
        if (n.camera && (!std::ranges::all_of(*n.camera, [](f32 v) { return std::isfinite(v); }) || (*n.camera)[2] <= 0))
            return std::unexpected(invalid(where + ": camera must be finite numbers with a positive distance"));
    }
    return {};
}

content::Result<std::string> encode_review(const Review& r, const std::filesystem::path& file) {
    if (auto valid = validate(r); !valid) return std::unexpected(valid.error());
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::setprecision(9);
    o << "vreview 1.0\nreview = 1;\ntitle = " << quote(r.title) << ";\ncreated = " << quote(r.created) << ";\n";
    if (r.range) o << "range = [" << (*r.range)[0] << ", " << (*r.range)[1] << "];\n";
    o << "summary = " << quote(r.summary) << ";\npick = " << quote(r.pick) << ";\nverdict = " << quote(r.verdict) << ";\n";
    o << "candidates = [\n";
    for (const auto& c : r.candidates)
        o << "    {\n        id = " << quote(c.id) << ";\n        label = " << quote(c.label)
          << ";\n        scene = " << quote(relative_scene(c.scene, file)) << ";\n        offset = " << c.offset
          << ";\n        about = " << quote(c.about) << ";\n        thoughts = " << quote(c.thoughts) << ";\n    },\n";
    o << "];\nnotes = [\n";
    for (const auto& n : r.notes) {
        o << "    {\n        id = " << n.id << ";\n        candidate = " << quote(n.candidate) << ";\n        time = " << n.time
          << ";\n        object = " << n.object << ";\n        object_name = " << quote(n.object_name) << ";\n";
        if (n.point) o << "        point = [" << n.point->x << ", " << n.point->y << ", " << n.point->z << "];\n";
        o << "        view = [" << n.view.x << ", " << n.view.y << "];\n        text = " << quote(n.text)
          << ";\n        created = " << quote(n.created) << ";\n        status = " << quote(n.status)
          << ";\n        reply = " << quote(n.reply) << ";\n";
        if (!n.clip.empty()) o << "        clip = " << quote(n.clip) << ";\n";
        if (n.camera) {
            o << "        camera = [";
            for (std::size_t k = 0; k < n.camera->size(); ++k) o << (k ? ", " : "") << (*n.camera)[k];
            o << "];\n";
        }
        o << "    },\n";
    }
    o << "];\n";
    auto result = std::move(o).str();
    // The writer must never produce a file its own reader rejects.
    if (auto check = parse_review(result, file); !check) return std::unexpected(check.error());
    return result;
}

content::Result<Review> parse_review(std::string_view source, const std::filesystem::path& file) {
    auto document = content::parse_document(source, {.limits = {.max_source_bytes = 16U << 20,
        .max_decoded_bytes = 64U << 20, .max_depth = 16, .max_string_bytes = 64U << 10}, .source_path = file});
    if (!document) return std::unexpected(document.error());
    if (document->kind() != "vreview") return std::unexpected(invalid("Not a review document (expected \"vreview 1.0\")"));
    const auto directory = directory_of(file);
    auto parsed = document->read([&](Reader& r) {
        if (r.get<u32>("review") != 1) fail_at(r, "review", "Unsupported review version");
        Review result;
        result.title = r.get<std::string>("title");
        result.created = r.get_or<std::string>("created", "");
        if (const auto range = r.optional<std::array<f32, 2>>("range")) result.range = *range;
        result.summary = r.get_or<std::string>("summary", "");
        result.pick = r.get_or<std::string>("pick", "");
        result.verdict = r.get_or<std::string>("verdict", "");
        for (const auto entry : r.child("candidates").elements()) {
            Candidate c;
            c.id = entry.get<std::string>("id");
            c.label = entry.get<std::string>("label");
            const auto scene = entry.get<std::string>("scene");
            if (scene.empty() || scene.find('\0') != std::string::npos) fail_at(entry, "scene", "scene must be a file path");
            c.scene = std::filesystem::path(scene).is_relative() ? (directory / scene).lexically_normal() : std::filesystem::path(scene);
            c.offset = entry.get_or<f32>("offset", 0);
            c.about = entry.get_or<std::string>("about", "");
            c.thoughts = entry.get_or<std::string>("thoughts", "");
            result.candidates.push_back(std::move(c));
        }
        if (r.view().child("notes"))
            for (const auto entry : r.child("notes").elements()) {
                Note n;
                n.id = entry.get<u32>("id");
                n.candidate = entry.get<std::string>("candidate");
                n.time = entry.get<f32>("time");
                n.object = entry.get_or<u32>("object", 0);
                n.object_name = entry.get_or<std::string>("object_name", "");
                if (const auto point = entry.optional<Vec3>("point")) n.point = *point;
                n.view = entry.get_or<Vec2>("view", Vec2{.5F, .5F});
                n.text = entry.get_or<std::string>("text", "");
                n.created = entry.get_or<std::string>("created", "");
                n.status = entry.get_or<std::string>("status", "open");
                n.reply = entry.get_or<std::string>("reply", "");
                n.clip = entry.get_or<std::string>("clip", "");
                // Six numbers; the four of earlier files looked at a height on his centre line.
                if (const auto camera = entry.optional<std::vector<f32>>("camera")) {
                    if (camera->size() == 6) n.camera = std::array{(*camera)[0], (*camera)[1], (*camera)[2], (*camera)[3], (*camera)[4], (*camera)[5]};
                    else if (camera->size() == 4) n.camera = std::array{(*camera)[0], (*camera)[1], (*camera)[2], 0.F, (*camera)[3], 0.F};
                    else fail_at(entry, "camera", "camera must be six numbers: yaw, pitch, distance and the point looked at");
                }
                result.notes.push_back(std::move(n));
            }
        return result;
    });
    if (!parsed) return std::unexpected(parsed.error());
    if (auto valid = validate(*parsed); !valid) {
        auto error = valid.error();
        error.path = file;
        return std::unexpected(std::move(error));
    }
    return parsed;
}

content::Result<Review> read_review(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::unexpected(io_failure(file, "Cannot open the review"));
    std::error_code error;
    if (const auto size = std::filesystem::file_size(file, error); !error && size > max_file_bytes)
        return std::unexpected(diagnostic(content::ErrorCode::input_too_large, "The review is larger than 16 MiB", file));
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) return std::unexpected(io_failure(file, "Cannot read the review"));
    return parse_review(std::move(buffer).str(), file);
}

std::optional<FileStamp> stamp_of(const std::filesystem::path& file) {
    struct stat status{};
    if (::stat(file.c_str(), &status) != 0) return {};
    return stamp(status);
}

content::Result<FileStamp> save_review(const std::filesystem::path& file, const Review& r,
                                      const std::optional<std::filesystem::path>& backup) {
    auto bytes = encode_review(r, file);
    if (!bytes) return std::unexpected(bytes.error());
    std::error_code error;
    if (backup && !std::filesystem::copy_file(file, *backup, std::filesystem::copy_options::none, error))
        return std::unexpected(diagnostic(content::ErrorCode::io_error,
            "Cannot preserve the existing review as " + backup->string() + ": " + error.message(), file));
    auto target = std::filesystem::weakly_canonical(std::filesystem::absolute(file), error);
    if (error) target = std::filesystem::absolute(file);
    struct stat existing{};
    const bool replacing = ::stat(target.c_str(), &existing) == 0;
    if (replacing && !S_ISREG(existing.st_mode)) return std::unexpected(io_failure(file, "Not a regular file", EINVAL));
    // Created 0666 less the umask, like any new file; a replaced file keeps its permissions.
    std::string temporary;
    int fd = -1;
    std::random_device random;
    for (unsigned attempt = 0; fd < 0 && attempt < 16; ++attempt) {
        temporary = (target.parent_path() / std::format(".{}.{:08x}.tmp", target.filename().string(), random())).string();
        fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
        if (fd < 0 && errno != EEXIST) return std::unexpected(io_failure(file, "Cannot create a temporary file beside the review"));
    }
    if (fd < 0) return std::unexpected(io_failure(file, "Cannot create a temporary file beside the review", EEXIST));
    struct Cleanup {
        int fd;
        std::string path;
        ~Cleanup() {
            if (fd >= 0) ::close(fd);
            if (!path.empty()) ::unlink(path.c_str());
        }
    } cleanup{fd, temporary};
    for (std::size_t offset{}; offset != bytes->size();) {
        const auto count = ::write(fd, bytes->data() + offset, bytes->size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return std::unexpected(io_failure(file, "Cannot write the review", count ? errno : EIO));
        offset += static_cast<std::size_t>(count);
    }
    if (replacing && ::fchmod(fd, existing.st_mode & 07777) != 0)
        return std::unexpected(io_failure(file, "Cannot keep the review's permissions"));
    if (::fsync(fd) != 0) return std::unexpected(io_failure(file, "Cannot flush the review"));
    // Renaming keeps the inode and its modification time, so this is the stamp the file will have.
    struct stat written{};
    if (::fstat(fd, &written) != 0) return std::unexpected(io_failure(file, "Cannot inspect the written review"));
    const auto closed = ::close(std::exchange(cleanup.fd, -1));
    if (closed != 0) return std::unexpected(io_failure(file, "Cannot close the review"));
    if (::rename(temporary.c_str(), target.c_str()) != 0) return std::unexpected(io_failure(file, "Cannot replace the review"));
    cleanup.path.clear();
    // The contents were flushed before the rename, the commit point; syncing the directory is best effort.
    if (const int directory = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); directory >= 0) {
        (void)::fsync(directory);
        ::close(directory);
    }
    return stamp(written);
}

Review merge(const Review& base, const Review& mine, const Review& theirs) {
    Review out;
    out.title = side(base.title, mine.title, theirs.title);
    out.created = side(base.created, mine.created, theirs.created);
    out.range = side(base.range, mine.range, theirs.range);
    out.summary = side(base.summary, mine.summary, theirs.summary);
    out.pick = side(base.pick, mine.pick, theirs.pick);
    out.verdict = side(base.verdict, mine.verdict, theirs.verdict);
    std::vector<Candidate> same_ids;
    out.candidates = merged(base.candidates, mine.candidates, theirs.candidates, &Candidate::id, same_ids);
    std::vector<Note> renumbered;
    out.notes = merged(base.notes, mine.notes, theirs.notes, &Note::id, renumbered);
    for (auto& note : renumbered) {
        note.id = out.next_note_id();
        out.notes.push_back(std::move(note));
    }
    std::erase_if(out.notes, [&](const Note& n) { return !out.find(n.candidate); });
    if (!out.pick.empty() && !out.find(out.pick)) out.pick.clear();
    return out;
}
} // namespace review
