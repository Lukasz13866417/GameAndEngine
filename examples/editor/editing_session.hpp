#pragma once

#include "document_patch.hpp"
#include "edit_clipboard.hpp"
#include "scene_file.hpp"
#include "authoring_limits.hpp"
#include "transform_pivot.hpp"
#include "scale_limits.hpp"
#include <deque>

namespace editor_example {

enum class EditGesture { none, move, rotation, scale, vertices, camera, world_bounds, region, mesh_draft, mesh_transform };
enum class MeshOperation { fill, subdivide, align };
struct MeshOperationSettings {
    vng::u32 levels{1}; // Repeated midpoint subdivision of the selected patch.
    vng::f32 strength{1}; // Align-to-line blend; anchors never move.
};

// Values leaving the authoring boundary. The application decides how to display
// and deliver them; the session knows nothing about widgets, IPC or the GPU.
struct EditNotice {
    vng::u64 revision{};
    DocumentChanges changes;
    bool discontinuous{};
};

// One authority for authored changes. Read access cannot mutate the document;
// private navigation remains independently writable. A gesture previews locally,
// publishes precise changes, and makes exactly one undo entry on commit.
class EditingSession final {
public:
    explicit EditingSession(State initial, SceneFile file = {});
    EditingSession(const EditingSession&) = delete;
    EditingSession& operator=(const EditingSession&) = delete;

    [[nodiscard]] const State& state() const noexcept { return state_; }
    [[nodiscard]] ViewportState& viewport() noexcept { return state_.viewport; }
    // Explicit authoring intent, separate from both the playhead and object
    // selection. Never serialized, sent to the worker, or restored by loading.
    void select_keyframe(std::optional<vng::f32> time) noexcept { selected_keyframe_ = time; }
    [[nodiscard]] bool can_edit_scene_pose() const;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool active(EditGesture kind) const noexcept;
    [[nodiscard]] vng::u32 active_object() const noexcept;
    [[nodiscard]] std::optional<EditNotice> take_changes();

    // Preferences are not authored content. Existing tracks, load and history
    // are preserved even above this budget; only adding more tracks is blocked.
    [[nodiscard]] unsigned timeline_track_limit() const noexcept { return track_limit_; }
    [[nodiscard]] vng::content::Result<void> timeline_track_limit(unsigned);
    [[nodiscard]] unsigned instance_limit() const noexcept { return instance_limit_; }
    [[nodiscard]] vng::content::Result<void> instance_limit(unsigned);

    [[nodiscard]] vng::content::Result<bool> undo();
    [[nodiscard]] vng::content::Result<bool> redo();

    [[nodiscard]] vng::content::Result<void> begin_move(vng::u32 primary, std::span<const vng::u32> selection = {});
    [[nodiscard]] vng::content::Result<void> begin_rotation(vng::u32 primary, std::span<const vng::u32> selection = {}, TransformPivot = {});
    [[nodiscard]] vng::content::Result<void> begin_attitude(vng::u32 primary, std::span<const vng::u32> selection = {}, TransformPivot = {});
    [[nodiscard]] vng::content::Result<void> begin_scale(vng::u32 primary, std::span<const vng::u32> selection = {}, bool with_axes = false);
    [[nodiscard]] vng::content::Result<void> begin_vertices(BlueprintId, std::span<const vng::u32>);
    [[nodiscard]] vng::content::Result<void> begin_camera();
    [[nodiscard]] vng::content::Result<void> begin_world_bounds();
    [[nodiscard]] vng::content::Result<bool> world_bounds(const WorldBounds&);
    [[nodiscard]] vng::content::Result<vng::u32> add_region(RegionShape, vng::Vec3 center, vng::f32 radius);
    [[nodiscard]] vng::content::Result<bool> erase_region(vng::u32);
    [[nodiscard]] vng::content::Result<void> begin_region(vng::u32);
    [[nodiscard]] vng::content::Result<bool> region(const Region&);
    // Point gestures retain exact instance/point IDs in history and preview patches.
    [[nodiscard]] vng::content::Result<void> begin_region_points(vng::u32, std::span<const vng::u32>);
    [[nodiscard]] vng::content::Result<bool> region_points(std::span<const RegionPointEdit>);
    // Movement is absolute for the primary object, delta-from-start for vertices.
    [[nodiscard]] vng::content::Result<bool> move(vng::Vec3);
    [[nodiscard]] vng::content::Result<bool> rotate(vng::Vec3);
    // World-space delta from gesture start; respects shared/individual/custom pivots.
    [[nodiscard]] vng::content::Result<bool> rotate_by(vng::Vec3);
    // Shared/custom pivot: use the primary's body axis for a rigid group turn.
    // Individual pivot: use each instance's own blueprint-relative axis.
    // 0=yaw, 1=pitch, 2=roll; angle is from gesture start, in degrees.
    [[nodiscard]] vng::content::Result<bool> attitude(vng::u32 axis, vng::f64 degrees);
    [[nodiscard]] vng::content::Result<bool> scale(vng::f32, std::optional<vng::f32> tool_maximum = {});
    // Mouse-relative factor; axis -1 means uniform, 0..2 means local X/Y/Z.
    [[nodiscard]] vng::content::Result<bool> scale_factor(vng::f32, int axis = -1, ScaleLimits limits = {});
    [[nodiscard]] vng::content::Result<bool> move_vertices(vng::Vec3 delta);
    [[nodiscard]] vng::content::Result<bool> vertices(std::span<const VertexPosition>);
    [[nodiscard]] vng::content::Result<bool> camera(const CameraPose&);
    [[nodiscard]] vng::content::Result<bool> commit();
    [[nodiscard]] vng::content::Result<bool> cancel();

    [[nodiscard]] vng::content::Result<vng::u32> import_mesh(const std::filesystem::path&);
    [[nodiscard]] vng::content::Result<vng::u32> import_asset(const std::filesystem::path&);
    [[nodiscard]] vng::content::Result<vng::u32> instantiate(BlueprintId);
    [[nodiscard]] vng::content::Result<bool> erase_instances(std::span<const vng::u32>);
    [[nodiscard]] vng::content::Result<bool> apply_mesh(BlueprintId);
    // A procedural blueprint tool replaces its working copy, never the
    // published scene geometry. Completion is tied to the revision it read.
    [[nodiscard]] vng::content::Result<bool> replace_mesh_draft(BlueprintId, vng::u64 expected_revision, vng::editor::EditableMesh);
    // Async blueprint-part gestures: each completed preview is narrow, and
    // commit/cancel owns one history entry for the entire drag.
    [[nodiscard]] vng::content::Result<void> begin_mesh_draft_edit(BlueprintId);
    [[nodiscard]] vng::content::Result<void> begin_mesh_transform(BlueprintId);
    [[nodiscard]] vng::content::Result<bool> mesh_transform(vng::Mat4);
    [[nodiscard]] vng::content::Result<bool> preview_mesh_draft(vng::u64 expected_revision, vng::editor::EditableMesh);
    [[nodiscard]] vng::content::Result<bool> mesh_operation(BlueprintId, MeshOperation,
        std::span<const vng::u32> vertices, std::span<const vng::gfx::Edge> edges = {}, MeshOperationSettings = {});
    // Adjust only the immediately preceding operation, from its original mesh.
    // A revision token prevents replacing a newer edit, load, undo or redo.
    [[nodiscard]] vng::content::Result<bool> adjust_mesh_operation(vng::u64 revision, MeshOperationSettings);
    [[nodiscard]] bool can_adjust_mesh_operation(vng::u64 revision) const;
    [[nodiscard]] vng::content::Result<bool> translate_vertices(BlueprintId, std::span<const vng::u32>, vng::Vec3 delta);

    [[nodiscard]] vng::content::Result<bool> add_keyframe(vng::f32);
    [[nodiscard]] vng::content::Result<bool> apply_keyframe_range(vng::f32 first, vng::f32 last,
                                                               std::span<const KeyframeChange>);
    [[nodiscard]] vng::content::Result<bool> update_keyframe(vng::f32 from, vng::f32 to, std::string,
        std::span<const KeyframeValue>);
    [[nodiscard]] vng::content::Result<bool> erase_keyframes(std::span<const vng::f32>);
    [[nodiscard]] vng::content::Result<bool> duration(vng::f32);
    [[nodiscard]] vng::content::Result<void> copy_instances(std::span<const vng::u32>);
    [[nodiscard]] vng::content::Result<void> copy_keyframes(std::span<const vng::f32>);
    [[nodiscard]] vng::content::Result<EditClipboard::Pasted> paste();

    [[nodiscard]] vng::content::Result<bool> set_transform(vng::u32, vng::Vec3 rotation, vng::f32 scale,
        std::optional<vng::Vec3> axis_scale = {});
    // Camera authoring at the selected keyframe: place a camera where a pose
    // looks from, and choose the camera the simulation uses from here on.
    [[nodiscard]] vng::content::Result<bool> set_camera(vng::u32, const CameraPose&);
    [[nodiscard]] vng::content::Result<bool> set_active_camera(vng::u32);
    [[nodiscard]] vng::content::Result<void> begin_remote(vng::u64 generation);
    [[nodiscard]] bool awaiting_remote() const noexcept { return remote_.has_value(); }
    void abandon_remote() noexcept { remote_.reset(); }
    [[nodiscard]] vng::content::Result<bool> accept_remote(vng::u64 generation, const DocumentPatch&);

    [[nodiscard]] const std::optional<std::filesystem::path>& path() const noexcept { return file_.path(); }
    [[nodiscard]] vng::content::Result<void> load(const std::filesystem::path&);
    [[nodiscard]] vng::content::Result<void> save();
    [[nodiscard]] vng::content::Result<void> save_as(const std::filesystem::path&, bool replace_existing = false);

private:
    struct Bookmark {
        BlueprintId blueprint;
        vng::u32 object, vertex;
        vng::f32 time;
        bool paused, weld;
    };
    struct Checkpoint {
        std::variant<Document, DocumentPatch> value;
        DocumentChanges scope;
        std::map<BlueprintId, bool> drafts;
        Bookmark bookmark;
        vng::u64 content{};
    };
    struct Gesture {
        EditGesture kind;
        Checkpoint before;
        std::vector<vng::u32> objects;
        std::vector<vng::Vec3> origins;
        std::vector<vng::timeline::Value> sampled;
        std::optional<CameraPose> camera_origin;
        BlueprintId blueprint{};
        bool different{};
        std::vector<std::array<vng::Vec3,3>> attitude_axes{};
        TransformPivot pivot{};
        std::vector<InstanceCenter> centers{};
        vng::editor::MeshChanges mesh_changes{};
    };
    struct Remote { vng::u64 generation, revision; };
    State state_;
    SceneFile file_;
    EditClipboard clipboard_;
    std::deque<Checkpoint> undo_, redo_;
    std::optional<Gesture> gesture_;
    std::optional<Remote> remote_;
    std::optional<EditNotice> notice_;
    struct MeshAdjustment {
        BlueprintId blueprint;
        MeshOperation operation;
        vng::editor::EditableMesh before;
        std::vector<vng::u32> vertices;
        std::vector<vng::gfx::Edge> edges;
        MeshOperationSettings settings;
        vng::u64 revision;
    };
    std::optional<MeshAdjustment> mesh_adjustment_;
    std::optional<vng::f32> selected_keyframe_;
    vng::u64 content_{1}, saved_{1}, next_content_{2};
    unsigned track_limit_{default_timeline_track_limit};
    unsigned instance_limit_{default_instance_limit};

    [[nodiscard]] vng::content::Result<void> available() const;
    [[nodiscard]] vng::content::Result<void> writable() const;
    [[nodiscard]] vng::content::Result<Checkpoint> capture(const DocumentChanges&) const;
    [[nodiscard]] vng::content::Result<void> restore(const Checkpoint&, bool bookmark);
    [[nodiscard]] vng::content::Result<bool> differs(const Checkpoint&) const;
    [[nodiscard]] vng::content::Result<void> check_track_growth(std::size_t before, std::size_t after) const;
    [[nodiscard]] vng::content::Result<void> check_track_growth(const Checkpoint&) const;
    void publish(const DocumentChanges&, bool discontinuous = false);
    void remember(Checkpoint);
    [[nodiscard]] vng::content::Result<bool> replace(State, const DocumentChanges&);
    [[nodiscard]] vng::content::Result<bool> replace_animation(State);
    [[nodiscard]] vng::content::Result<void> begin(EditGesture, DocumentChanges, std::vector<vng::u32> = {});
    [[nodiscard]] vng::content::Result<bool> updated(const Checkpoint& before_update);
    [[nodiscard]] vng::content::Result<bool> rotations(std::span<const vng::Vec3>);
    [[nodiscard]] vng::content::Result<void> compact_mesh_gesture();
    [[nodiscard]] vng::content::Result<bool> traverse(std::deque<Checkpoint>& from, std::deque<Checkpoint>& to);
};
} // namespace editor_example
