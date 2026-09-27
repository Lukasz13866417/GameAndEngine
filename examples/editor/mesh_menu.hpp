#pragma once

#include "component_debug.hpp"
#include <vng/ui/ui.hpp>

namespace editor_example {
enum class MeshAction { fill, subdivide, align, hide, reveal };

// Counts are enough for this leaf. It cannot read the mesh, session, worker,
// selection owner, or the parent panel. Context is valid for this call only.
struct MeshMenuContext {
    std::span<const vng::input::Event> events;
    bool reveal_available{};
    bool accept_input{true};
};
class MeshMenu final {
public:
    struct Vertices { std::size_t selected{}; };
    struct Edges { std::size_t selected{}; };
    struct Faces { std::size_t selected{}; };
    struct Inactive {};

    explicit MeshMenu(vng::ui::Container);
    MeshMenu(const MeshMenu&) = delete;
    MeshMenu& operator=(const MeshMenu&) = delete;
    void open(vng::Vec2 at, vng::Vec2 screen, std::optional<vng::ui::Rect> viewport);
    void close();
    [[nodiscard]] bool opened() const { return open_; }
    [[nodiscard]] DebugReport debug_report() const;
    [[nodiscard]] std::string debug_string() const { return debug_report().string(); }

private:
    friend class MeshToolsUI;
    std::optional<MeshAction> handle(const Vertices&, const MeshMenuContext&);
    std::optional<MeshAction> handle(const Edges&, const MeshMenuContext&);
    std::optional<MeshAction> handle(const Faces&, const MeshMenuContext&);
    std::optional<MeshAction> handle(const Inactive&, const MeshMenuContext&);
    std::optional<MeshAction> input(const MeshMenuContext&);
    void controls(bool fill, bool subdivide, bool align, bool hide, bool reveal);
    void received(std::string_view situation, std::size_t selected, const MeshMenuContext&);

    vng::ui::Container host_;
    vng::ui::Button fill_, subdivide_, align_, hide_, reveal_;
    bool open_{}, accepts_input_{}, reveal_available_{};
    std::array<bool, 5> enabled_{};
    std::size_t selected_{};
    std::string_view situation_{"Inactive"};
    std::optional<MeshAction> last_action_;
};
} // namespace editor_example
