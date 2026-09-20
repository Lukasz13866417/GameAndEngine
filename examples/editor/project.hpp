#pragma once

#include "environment.hpp"
#include "camera_limits.hpp"
#include "world_bounds.hpp"
#include "regions.hpp"
#include "document_changes.hpp"

#include <vng/editor/mesh.hpp>
#include <vng/gfx/camera.hpp>
#include <vng/render/view.hpp>
#include <vng/timeline/timeline.hpp>
#include <map>
#include <array>
#include <span>
#include <variant>

namespace editor_example {
inline constexpr vng::f32 camera_max_pitch = 89.5F;
inline constexpr vng::f32 camera_target_limit = scene_coordinate_limit;
inline constexpr vng::f32 camera_vertical_fov = 43;
enum class ViewMode { scene, mesh, sun };
struct InstanceTransform {
    vng::Vec3 position{};
    // XYZ Euler angles in degrees. Local points use T * Rz * Ry * Rx * S.
    vng::Vec3 rotation{};
    vng::f32 scale{1};
    // Local-axis proportions, independent of the overall size above. Keeping
    // these separate lets uniform scale preserve a deliberately stretched shape.
    vng::Vec3 axis_scale{1,1,1};
    friend bool operator==(const InstanceTransform&, const InstanceTransform&) = default;
};
struct MeshSettings {
    vng::f32 brightness{1};
    bool visible{true}, wireframe{};
    friend bool operator==(const MeshSettings&, const MeshSettings&) = default;
};
struct SunSettings {
    vng::f32 radius{1.1F}, displacement{1}, bloom{0.24F};
    bool white_spots{}, visible{true};
    friend bool operator==(const SunSettings&, const SunSettings&) = default;
};
// A scene camera used by the actual simulation (independent Play and demos),
// never the editor's private viewing camera. Its transform is the eye and
// look direction, following the mesh -Z heading convention; roll is stored but
// not rendered. focus is the orbit pivot distance, which also sizes the far
// plane. Only one camera may be active at any timestamp; the first camera is
// used when none is marked active.
struct CameraSettings {
    vng::f32 zoom{1}, focus{8};
    bool active{}, visible{true};
    friend bool operator==(const CameraSettings&, const CameraSettings&) = default;
};
// Reserved builtin identities do not steal ID 3 from existing imported assets.
enum class BlueprintId : vng::u32 { mesh = 1, sun = 2, camera = 0xfffffffdU, region = 0xfffffffeU };
inline constexpr vng::u32 first_reserved_blueprint = static_cast<vng::u32>(BlueprintId::camera);
enum class BlueprintKind { mesh, sun, region, camera };
struct Blueprint {
    BlueprintId id;
    std::string_view name;
    BlueprintKind kind;
};
struct MeshBlueprint {
    BlueprintId id;
    std::string name;
    vng::editor::EditableMesh geometry;
    MeshSettings settings{};
};
// A portable preset refers to a compiled effect through its settings type.
// Defaults belong to the blueprint; every instance has an independent copy.
struct EffectBlueprint {
    BlueprintId id;
    std::string name;
    SunSettings settings;
    friend bool operator==(const EffectBlueprint&, const EffectBlueprint&) = default;
};
struct SceneInstance {
    vng::u32 id{};
    BlueprintId blueprint{BlueprintId::mesh};
    std::string name;
    std::variant<MeshSettings, SunSettings, RegionSettings, CameraSettings> settings;
    InstanceTransform transform{};
    friend bool operator==(const SceneInstance&, const SceneInstance&) = default;
};
struct CameraPose {
    vng::f32 yaw{15}, pitch{12}, distance{8};
    vng::Vec3 target{};
    // Optical magnification, independent of the eye/pivot orbit distance.
    vng::f32 zoom{1};
    friend bool operator==(const CameraPose&, const CameraPose&) = default;
};
[[nodiscard]] bool valid_camera_pose(const CameraPose&);
// Authored data and its ordered mutation revision. Viewport navigation never
// writes this revision. GPU realizations belong to Runtime, not the document.
struct MeshPlacement {
    vng::Mat4 applied{vng::Mat4::identity()};
    std::optional<vng::Mat4> draft;
    friend bool operator==(const MeshPlacement&, const MeshPlacement&) = default;
};
struct Document {
    vng::u64 revision{1};
    vng::editor::EditableMesh mesh;
    // Blueprint data outlives every instance. Geometry edits intentionally
    // modify this shared mesh asset; transforms belong to individual instances.
    MeshSettings mesh_blueprint{};
    SunSettings sun_blueprint{};
    // Built-in IDs 1/2 remain stable in old scenes. Imported assets use their
    // own immutable identity and independently editable geometry/settings.
    std::vector<MeshBlueprint> mesh_assets{};
    std::vector<EffectBlueprint> effect_assets{};
    // Authored working copies, separate from the published blueprint geometry
    // above. Scene instances never resolve through this table. Saved projects
    // preserve drafts without implicitly publishing them.
    std::map<BlueprintId, vng::editor::EditableMesh> mesh_drafts{};
    // Lightweight authoring transforms; geometry remains in its original coordinates.
    std::map<BlueprintId, MeshPlacement> mesh_placements{};
    vng::u32 next_blueprint_id{3};
    std::vector<SceneInstance> instances{
        {1, BlueprintId::mesh, "Mesh", MeshSettings{}, {{1.7F, 0, 0}, {}, .7F}},
        {2, BlueprintId::sun, "Sun", SunSettings{}, {{-1.6F, 0, 0}, {}, 1}}};
    vng::u32 next_instance_id{3};
    CameraPose animation_camera{};
    EnvironmentSettings environment{};
    WorldBounds world_bounds{};
    vng::timeline::Timeline timeline{};
    vng::f32 timeline_duration{10};
    std::map<vng::f32, std::string> keyframe_names{};
};
// Private editor state. Its independently sequenced, absolute requests may be
// replaced by a newer request without dropping an authored document edit.
struct ViewportState {
    vng::u64 sequence{1};
    ViewMode mode{ViewMode::scene};
    // Blueprint inspection is independent of the selected scene instance.
    BlueprintId inspected_mesh{BlueprintId::mesh};
    vng::u32 selected_object{2}, selected_vertex{};
    bool weld{true}, paused{true};
    vng::f32 time{0};
    CameraPose editor_camera{};
    bool pilot_camera{};
    bool smooth_zoom{};
    bool show_regions{}, show_world_bounds{}; // Private preview decoration, never an authored pose.
    bool gizmo_only{}; // Hide the active instance surface in editor preview only.
    [[nodiscard]] bool hides_surface(vng::u32 instance) const {
        return gizmo_only && mode==ViewMode::scene && instance!=0 && instance==selected_object;
    }
    friend bool operator==(const ViewportState&, const ViewportState&) = default;
};
// UI/session assembly; neither part owns the other. Transport uses explicit
// document updates and viewport requests, not a global scene/view revision.
struct State {
    Document document;
    ViewportState viewport{};
};
[[nodiscard]] inline std::span<const SceneInstance> scene_instances(const State& state) {
    return state.document.instances;
}
[[nodiscard]] std::vector<Blueprint> blueprint_catalog(const State&);
[[nodiscard]] bool is_mesh_blueprint(const State&, BlueprintId);
[[nodiscard]] const MeshSettings* mesh_blueprint_settings(const State&, BlueprintId);
[[nodiscard]] vng::editor::EditableMesh* mesh_geometry(State&, BlueprintId);
[[nodiscard]] const vng::editor::EditableMesh* mesh_geometry(const State&, BlueprintId);
// Edit/patch target: draft when present, published geometry otherwise. Creating
// a draft is explicit, so read-only inspection never copies a mesh.
[[nodiscard]] vng::editor::EditableMesh* mesh_edit_geometry(State&, BlueprintId);
[[nodiscard]] const vng::editor::EditableMesh* mesh_edit_geometry(const State&, BlueprintId);
[[nodiscard]] const vng::editor::EditableMesh* mesh_view_geometry(const State&, BlueprintId);
[[nodiscard]] vng::content::Result<bool> begin_mesh_draft(State&, BlueprintId);
[[nodiscard]] vng::content::Result<bool> apply_mesh_draft(State&, BlueprintId);
[[nodiscard]] bool discard_mesh_draft(State&, BlueprintId);
[[nodiscard]] bool has_mesh_draft(const State&, BlueprintId);
[[nodiscard]] vng::Mat4 mesh_placement(const State&, BlueprintId, bool draft);
[[nodiscard]] vng::content::Result<State> bake_mesh_placements(const State&);
[[nodiscard]] vng::editor::EditableMesh* instance_mesh(State&, vng::u32);
[[nodiscard]] const vng::editor::EditableMesh* instance_mesh(const State&, vng::u32);
struct MeshTarget {
    BlueprintId blueprint;
    std::optional<vng::u32> instance;
    friend bool operator==(const MeshTarget&, const MeshTarget&) = default;
};
// Mesh view edits the explicit blueprint, even without a live instance. Scene
// view edits only its selected mesh instance; Sun/empty selection has no target.
[[nodiscard]] std::optional<MeshTarget> mesh_target(const State&);
[[nodiscard]] vng::editor::EditableMesh* editable_mesh(State&);
[[nodiscard]] const vng::editor::EditableMesh* editable_mesh(const State&);
// Select and frame an exact blueprint without changing scene selection.
[[nodiscard]] vng::content::Result<void> inspect_mesh(State&, BlueprintId);
[[nodiscard]] vng::content::Result<void> inspect_mesh(const State&, ViewportState&, BlueprintId);
[[nodiscard]] vng::content::Result<vng::u32> import_mesh(State&, const std::filesystem::path&);
[[nodiscard]] vng::content::Result<vng::u32> import_asset(State&, const std::filesystem::path&);
[[nodiscard]] const SunSettings* effect_blueprint_settings(const State&, BlueprintId);
[[nodiscard]] SceneInstance* find_instance(State&, vng::u32);
[[nodiscard]] const SceneInstance* find_instance(const State&, vng::u32);
[[nodiscard]] InstanceTransform* instance_transform(State&, vng::u32);
[[nodiscard]] const InstanceTransform* instance_transform(const State&, vng::u32);
[[nodiscard]] MeshSettings* mesh_settings(State&, vng::u32);
[[nodiscard]] const MeshSettings* mesh_settings(const State&, vng::u32);
[[nodiscard]] SunSettings* sun_settings(State&, vng::u32);
[[nodiscard]] const SunSettings* sun_settings(const State&, vng::u32);
[[nodiscard]] RegionSettings* region_settings(State&, vng::u32);
[[nodiscard]] const RegionSettings* region_settings(const State&, vng::u32);
[[nodiscard]] CameraSettings* camera_settings(State&, vng::u32);
[[nodiscard]] const CameraSettings* camera_settings(const State&, vng::u32);
[[nodiscard]] bool is_camera_instance(const State&, vng::u32);
[[nodiscard]] bool has_camera(const State&); // any scene camera instance
// The orbit pose a camera instance renders with (roll ignored), and the
// inverse placement. Both are pure value conversions without timeline access.
[[nodiscard]] CameraPose camera_pose(const SceneInstance& evaluated);
void place_camera(SceneInstance&, const CameraPose&);
// At every keyed time at most one camera may be active.
[[nodiscard]] vng::content::Result<void> validate_active_cameras(const State&);
[[nodiscard]] Regions region_snapshot(const State&); // authored local boundaries
[[nodiscard]] Regions region_world_snapshot(const State&); // sampled scene cages
[[nodiscard]] std::optional<Region> region_world_snapshot(const State&, vng::u32); // one visible instance
[[nodiscard]] Region region_to_local(const State&, const Region& world);
[[nodiscard]] Region region_to_world(const State&, const Region& local);
[[nodiscard]] bool is_mesh_instance(const State&, vng::u32);
// A kind query never chooses another mesh when a non-mesh instance is selected.
// Blueprint inspection has no scene instance; use mesh_target for that view.
[[nodiscard]] const SceneInstance* view_instance(const State&, BlueprintKind);
[[nodiscard]] bool instance_in_view(const State&, const SceneInstance&);
[[nodiscard]] std::string object_name(const State&, vng::u64);
// Mutations are transactional; the caller owns history, dirty state and revision.
[[nodiscard]] vng::content::Result<vng::u32> instantiate(State&, BlueprintId);
[[nodiscard]] vng::content::Result<void> erase_instance(State&, vng::u32);
[[nodiscard]] vng::content::Result<std::string> encode(const State&);
// Authored scene serialization excludes the private inspection camera and pilot mode.
[[nodiscard]] vng::content::Result<std::string> encode_scene(const State&);
[[nodiscard]] vng::content::Result<State> decode(std::string_view);
[[nodiscard]] inline CameraPose& view_camera(State& state) noexcept {
    return state.viewport.pilot_camera ? state.document.animation_camera : state.viewport.editor_camera;
}
[[nodiscard]] inline const CameraPose& view_camera(const State& state) noexcept {
    return state.viewport.pilot_camera ? state.document.animation_camera : state.viewport.editor_camera;
}
[[nodiscard]] vng::gfx::Camera camera(const State&);
// Zero preserves automatic clipping for standalone demos; editor preferences
// supply an explicit far plane, independent of the orbit pivot or optical zoom.
[[nodiscard]] vng::gfx::Camera camera(const CameraPose&, ViewMode, vng::f32 maximum_distance = 0);
[[nodiscard]] vng::Mat4 mesh_transform(const State&);
[[nodiscard]] vng::Mat4 mesh_transform(const State&, vng::f32 time);
// Isolated mesh view shows blueprint geometry with an identity transform.
[[nodiscard]] vng::Mat4 mesh_transform(ViewMode, const InstanceTransform&);
[[nodiscard]] vng::Mat4 mesh_transform(const State&, BlueprintId, const InstanceTransform&);
[[nodiscard]] vng::Vec3 sun_position(const State&);
[[nodiscard]] vng::Vec3 sun_position(const State&, vng::f32 time);
// Normalized top-left viewport coordinates; invisible/behind-camera vertices
// return nullopt. Picking is paired with the authored frame revision by the UI.
[[nodiscard]] std::optional<vng::Vec3> project_vertex(const State&, vng::u32, vng::Extent2D, const vng::gfx::Camera* = nullptr);
// Batch projection builds camera and model transforms once per pass.
[[nodiscard]] std::vector<std::optional<vng::Vec3>> project_vertices(const State&, vng::Extent2D, const vng::gfx::Camera* = nullptr, bool clip_to_viewport = true);
[[nodiscard]] std::optional<vng::u32> pick_vertex(const State&, vng::Vec2, vng::Extent2D,
                                                  vng::f32 radius_pixels = 12, const vng::gfx::Camera* = nullptr);
[[nodiscard]] vng::content::Result<void> save_new(const std::filesystem::path&, std::string_view);
[[nodiscard]] vng::content::Result<State> load_scene(const std::filesystem::path&);

} // namespace editor_example
