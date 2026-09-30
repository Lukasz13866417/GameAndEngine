#pragma once

#include "project.hpp"
#include <filesystem>
#include <optional>

namespace editor_example {

// The current scene filename belongs to the document session, not a text
// field in the UI. Save As changes it only after successful publication.
class SceneFile final {
public:
    [[nodiscard]] const std::optional<std::filesystem::path>& path() const noexcept { return path_; }
    [[nodiscard]] vng::content::Result<State> load(const std::filesystem::path&);
    [[nodiscard]] vng::content::Result<void> save(const State&);
    [[nodiscard]] vng::content::Result<void> save_as(const std::filesystem::path&, const State&,
                                                   bool replace_existing = false);
    // Saves a scene together with the standalone .vmesh it was authored from,
    // such as an Earth and its scene. Both are encoded and checked against the
    // editor's read limits before either is written, and the mesh is published
    // only after the scene, so a scene that cannot be saved leaves no new mesh.
    [[nodiscard]] vng::content::Result<void> save_as_with_mesh(const std::filesystem::path& scene, const State&,
                                                             const std::filesystem::path& mesh,
                                                             const vng::content::vmesh::Document&,
                                                             bool replace_existing = false);

private:
    std::optional<std::filesystem::path> path_;
};

// Linux persistence policy: absolute paths with canonical parent directories;
// final-component symlinks/nonregular files rejected. Same-directory temporary
// files are synced before atomic publication. New files are private (0600),
// replacements retain mode bits; ownership, ACLs, and xattrs are not copied.
// Rename is the commit point. Directory fsync afterward is best-effort: an
// error after commit cannot promise restoration of the previous directory entry.
} // namespace editor_example
