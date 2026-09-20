#pragma once
#include "project.hpp"
#include "component_transform.hpp"
#include "mesh_visibility.hpp"
#include <vng/ui/ui.hpp>
#include <vng/editor/selection.hpp>

namespace editor_example {
// Surface is a local preview mode: no component selection or editor overlay.
// It never overrides the mesh's own rendering (including authored wireframe).
enum class MeshSelectMode { vertex, edge, face, surface, whole };
enum class MeshAction { fill, subdivide, align, hide, reveal };
inline constexpr std::array whole_mesh_gizmos{GizmoMode::rotate,GizmoMode::scale,GizmoMode::free_rotate};
// Owns local component selection and temporary visibility. Neither authors a
// document change. Only the compact visibility mask crosses IPC, on change.
class MeshTools {
public:
    MeshTools(vng::ui::Container controls, vng::ui::Container popup);
    void sync(const State&);
    void mode(MeshSelectMode);
    [[nodiscard]] MeshSelectMode mode() const { return mode_; }
    [[nodiscard]] bool component_mode() const {
        return mode_==MeshSelectMode::vertex || mode_==MeshSelectMode::edge || mode_==MeshSelectMode::face;
    }
    [[nodiscard]] std::span<const vng::u32> selected() const { return selected_; }
    [[nodiscard]] std::span<const vng::gfx::Edge> all_edges() const { return edges_; }
    [[nodiscard]] vng::u64 selection_revision() const { return selection_revision_; }
    [[nodiscard]] vng::u64 topology_revision() const { return topology_revision_; }
    [[nodiscard]] vng::u64 visibility_revision() const { return visibility_revision_; }
    [[nodiscard]] const MeshVisibility& visibility() const { return visibility_; }
    [[nodiscard]] bool visible(MeshSelectMode,vng::u32) const;
    std::size_t hide_selected();
    std::size_t reveal_hidden();
    [[nodiscard]] bool xray() const { return xray_.value(); }
    void xray(bool enabled) { xray_.value(enabled); }
    [[nodiscard]] std::vector<vng::u32> vertices(const vng::editor::EditableMesh&, bool weld = false) const;
    [[nodiscard]] std::vector<vng::gfx::Edge> edges(const vng::editor::EditableMesh&) const;
    void select(vng::u32 element, bool extend);
    void select(std::span<const vng::u32>,vng::editor::SelectionMode);
    [[nodiscard]] std::vector<vng::u32> box(const State&,vng::ui::Rect normalized,vng::Extent2D,const vng::gfx::Camera&) const;
    void select_all(const vng::editor::EditableMesh&, bool clear = false);
    [[nodiscard]] std::optional<vng::u32> pick(const State&, vng::Vec2 normalized,
        vng::Extent2D, const vng::gfx::Camera&, vng::ui::Rect) const;
    void open(vng::Vec2 at, vng::Vec2 screen, std::optional<vng::ui::Rect> viewport = {});
    void close();
    [[nodiscard]] bool menu_open() const { return menu_open_; }
    [[nodiscard]] std::optional<MeshAction> poll(std::span<const vng::input::Event>);
    void reset();
    TransformKind transform_kind() const {return gizmo_transform_kind(transform_mode());}
    void transform_kind(TransformKind kind) {transform_mode(transform_gizmo(kind));}
    GizmoMode transform_mode() const {return mode_==MeshSelectMode::whole?whole_transform_.value():transform_.value();}
    void transform_mode(GizmoMode mode) {
        if(mode_==MeshSelectMode::whole) { if(mode!=GizmoMode::move)whole_transform_.value(mode); }
        else transform_.value(mode);
    }
    bool cycle(int direction);
private:
    vng::ui::Container popup_;
    vng::ui::Dropdown<MeshSelectMode> modes_;
    vng::ui::Dropdown<GizmoMode> transform_;
    vng::ui::Dropdown<GizmoMode> whole_transform_;
    vng::ui::Label summary_;
    vng::ui::Label transform_help_;
    vng::ui::Checkbox xray_;
    vng::ui::Button fill_, subdivide_, align_, hide_, reveal_;
    MeshSelectMode mode_{MeshSelectMode::vertex};
    std::vector<vng::u32> selected_;
    std::vector<vng::gfx::Edge> edges_;
    std::vector<vng::gfx::TriangleFace> faces_;
    std::optional<std::vector<vng::gfx::Edge>> explicit_edges_;
    std::size_t count_{};
    std::optional<BlueprintId> blueprint_;
    vng::u64 revision_{};
    vng::u64 selection_revision_{1}, topology_revision_{1}, summary_revision_{};
    MeshVisibility visibility_;
    std::vector<bool> visible_faces_,visible_edges_,visible_vertices_;
    vng::u64 visibility_revision_{1};
    void update_visibility();
    bool menu_open_{};
};
} // namespace editor_example
