#include "scene_file.hpp"
#include <vng/editor/limits.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace editor_example {
namespace {
namespace fs = std::filesystem;
namespace content = vng::content;
constexpr std::size_t max_scene_bytes = vng::editor::max_document_bytes;

auto invalid(const fs::path& path, std::string message,
             content::ErrorCode code = content::ErrorCode::io_error) {
    content::Diagnostic error;
    error.code = code;
    error.path = path;
    error.message = std::move(message);
    return std::unexpected(std::move(error));
}
auto system_error(const fs::path& path, std::string operation, int code = errno) {
    return invalid(path, std::move(operation) + ": " + std::strerror(code));
}

struct FileDescriptor final {
    int value{-1};
    explicit FileDescriptor(int fd = -1) : value(fd) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value(std::exchange(other.value, -1)) {}
    ~FileDescriptor() { if (value >= 0) ::close(value); }
};

struct Target final {
    fs::path path;
    std::string filename;
    FileDescriptor directory;
};

content::Result<Target> target(const fs::path& input) {
    const auto& native = input.native();
    if (native.empty() || native.find('\0') != std::string::npos || input.filename().empty() ||
        input.filename() == "." || input.filename() == "..")
        return invalid(input, "Scene path must name a nonempty regular file");
    std::error_code error;
    const auto absolute = fs::absolute(input, error);
    if (error) return invalid(input, "Cannot resolve scene path: " + error.message());
    const auto parent = fs::canonical(absolute.parent_path(), error);
    if (error) return invalid(input, "Cannot resolve scene directory: " + error.message());
    auto resolved = parent / absolute.filename();
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return system_error(resolved, "Cannot open scene directory");
    FileDescriptor owned_directory{directory};
    return Target{std::move(resolved), absolute.filename().native(), std::move(owned_directory)};
}

content::Result<std::optional<mode_t>> permissions(const Target& target) {
    struct stat status{};
    if (::fstatat(target.directory.value, target.filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return std::optional<mode_t>{};
        return system_error(target.path, "Cannot inspect scene destination");
    }
    if (!S_ISREG(status.st_mode))
        return invalid(target.path, "Scene paths cannot be symbolic links, directories, or special files");
    return std::optional<mode_t>{static_cast<mode_t>(status.st_mode & 07777)};
}

struct Temporary final {
    int directory;
    std::string name;
    FileDescriptor file;
    bool unpublished{true};
    Temporary(int parent, std::string filename, int fd)
        : directory(parent), name(std::move(filename)), file(fd) {}
    Temporary(const Temporary&) = delete;
    Temporary& operator=(const Temporary&) = delete;
    Temporary(Temporary&& other) noexcept
        : directory(other.directory), name(std::move(other.name)), file(std::move(other.file)),
          unpublished(std::exchange(other.unpublished, false)) {}
    ~Temporary() { if (unpublished) (void)::unlinkat(directory, name.c_str(), 0); }
};

content::Result<Temporary> temporary(const Target& target) {
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        std::array<unsigned char, 16> random{};
        std::size_t offset{};
        while (offset < random.size()) {
            const auto count = ::getrandom(random.data() + offset, random.size() - offset, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return system_error(target.path, "Cannot generate scene temporary filename", count ? errno : EIO);
            offset += static_cast<std::size_t>(count);
        }
        std::string name = ".vng-scene-";
        for (const auto byte : random) { name += digits[byte >> 4U]; name += digits[byte & 15U]; }
        name += ".tmp";
        const int fd = ::openat(target.directory.value, name.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) return Temporary{target.directory.value, std::move(name), fd};
        if (errno != EEXIST) return system_error(target.path, "Cannot create scene temporary file");
    }
    return invalid(target.path, "Cannot reserve a unique scene temporary file");
}

content::Result<void> write_atomic(Target& target, std::string_view bytes, bool replace_existing) {
    auto existing = permissions(target);
    if (!existing) return std::unexpected(existing.error());
    if (*existing && !replace_existing)
        return invalid(target.path, "Scene destination already exists; confirm replacement before Save As");
    auto temp = temporary(target);
    if (!temp) return std::unexpected(temp.error());
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto count = ::write(temp->file.value, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return system_error(target.path, "Cannot write scene temporary file", count ? errno : EIO);
        offset += static_cast<std::size_t>(count);
    }
    if (*existing && ::fchmod(temp->file.value, **existing) != 0)
        return system_error(target.path, "Cannot preserve scene file permissions");
    if (::fsync(temp->file.value) != 0) return system_error(target.path, "Cannot sync scene temporary file");
    const int fd = std::exchange(temp->file.value, -1);
    if (::close(fd) != 0) return system_error(target.path, "Cannot close scene temporary file");

    // Recheck type immediately before publication. rename never follows the
    // target entry even if another process races in a symlink after this check.
    auto checked = permissions(target);
    if (!checked) return std::unexpected(checked.error());
    if (*checked && !replace_existing)
        return invalid(target.path, "Scene destination appeared during Save As; it was not replaced");
    const auto renamed = replace_existing
        ? ::renameat(target.directory.value, temp->name.c_str(), target.directory.value, target.filename.c_str())
        : ::syscall(SYS_renameat2, target.directory.value, temp->name.c_str(),
                    target.directory.value, target.filename.c_str(), RENAME_NOREPLACE);
    if (renamed != 0) return system_error(target.path, "Cannot atomically publish scene file");
    temp->unpublished = false;
    // Contents were synced before publication. Treat rename as the commit
    // point; directory sync is best effort because reporting a pre-commit
    // failure here would falsely imply that the original remained untouched.
    (void)::fsync(target.directory.value);
    return {};
}

content::Result<State> read(const Target& target) {
    FileDescriptor fd{::openat(target.directory.value, target.filename.c_str(),
                               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (fd.value < 0) return system_error(target.path, "Cannot open scene file");
    struct stat status{};
    if (::fstat(fd.value, &status) != 0) return system_error(target.path, "Cannot inspect opened scene file");
    if (!S_ISREG(status.st_mode)) return invalid(target.path, "Scene input must be a regular file, not a directory or special file");
    if (status.st_size < 0 || static_cast<vng::u64>(status.st_size) > max_scene_bytes)
        return invalid(target.path, "Scene exceeds the 32 MiB editor limit", content::ErrorCode::input_too_large);
    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(status.st_size));
    std::array<char, 8192> buffer;
    while (true) {
        const auto count = ::read(fd.value, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return system_error(target.path, "Cannot read scene file");
        if (!count) break;
        if (static_cast<std::size_t>(count) > max_scene_bytes - bytes.size())
            return invalid(target.path, "Scene exceeds the 32 MiB editor limit", content::ErrorCode::input_too_large);
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    auto decoded = decode(bytes);
    if (!decoded) decoded.error().path = target.path;
    return decoded;
}
} // namespace

vng::content::Result<State> SceneFile::load(const std::filesystem::path& input) {
    auto resolved = target(input);
    if (!resolved) return std::unexpected(resolved.error());
    auto loaded = read(*resolved);
    if (!loaded) return std::unexpected(loaded.error());
    path_ = std::move(resolved->path);
    return loaded;
}

vng::content::Result<void> SceneFile::save(const State& state) {
    if (!path_) return invalid({}, "This scene has no file path yet; use Save As");
    return save_as(*path_, state, true);
}

vng::content::Result<void> SceneFile::save_as(const std::filesystem::path& destination,
                                           const State& state, bool replace_existing) {
    // Bake a disk copy, never the live document/history or a pointer-move sample.
    auto baked = bake_mesh_placements(state);
    if (!baked) return std::unexpected(baked.error());
    auto bytes = encode_scene(*baked);
    if (!bytes) return std::unexpected(bytes.error());
    auto resolved = target(destination);
    if (!resolved) return std::unexpected(resolved.error());
    auto saved = write_atomic(*resolved, *bytes, replace_existing);
    if (!saved) return saved;
    path_ = std::move(resolved->path);
    return {};
}
} // namespace editor_example
